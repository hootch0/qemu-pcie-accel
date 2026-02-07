/*
 * QEMU PCIe Accelerator Device - P2P DMA Engine
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This file implements PCIe peer-to-peer (P2P) DMA functionality:
 * - Multi-device P2P transfers with N:N concurrency
 * - Per-peer transfer tracking and rate limiting
 * - PASID/SVA support for shared virtual addressing
 * - Chunked transfers for large data movements
 * - Comprehensive error handling and validation
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "system/dma.h"

#include "hw/misc/pcie-accelerator.h"
#include "hw/misc/pcie-accelerator-regs.h"
#include "trace.h"

/* Maximum chunk size for P2P transfers (to avoid huge allocations) */
#define ACCEL_P2P_CHUNK_SIZE (64 * KiB)

/*
 * ===== P2P Peer Management =====
 */

/**
 * accel_find_p2p_peer - Find registered P2P peer by BDF
 * @n: Device state
 * @bdf: Bus:Device:Function identifier
 *
 * Returns: Peer structure if found, NULL otherwise
 */
AccelP2PPeer *accel_find_p2p_peer(PCIeAccel *n, uint16_t bdf)
{
    for (int i = 0; i < n->p2p.num_peers; i++) {
        if (n->p2p.peers[i].bdf == bdf && n->p2p.peers[i].enabled) {
            return &n->p2p.peers[i];
        }
    }
    return NULL;
}

/**
 * accel_register_p2p_peer - Register a new P2P peer device
 * @n: Device state
 * @bdf: Bus:Device:Function identifier of peer
 * @pdev: Peer PCI device pointer
 *
 * Registers a peer device for P2P DMA transfers. The peer's address space
 * is obtained for routing DMA operations.
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_register_p2p_peer(PCIeAccel *n, uint16_t bdf, PCIDevice *pdev)
{
    AccelP2PPeer *peer;

    /* Check if already registered - idempotent, return success */
    if (accel_find_p2p_peer(n, bdf)) {
        return 0;
    }

    /* Check capacity */
    if (n->p2p.num_peers >= n->p2p.max_peers) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Maximum P2P peers (%u) reached\n",
                      n->p2p.max_peers);
        return -1;
    }

    /* Find free slot */
    peer = NULL;
    for (int i = 0; i < ACCEL_MAX_P2P_PEERS; i++) {
        if (!n->p2p.peers[i].enabled) {
            peer = &n->p2p.peers[i];
            break;
        }
    }

    if (!peer) {
        return -1;
    }

    /* Initialize peer structure */
    peer->bdf = bdf;
    peer->pci_dev = pdev;
    peer->as = pci_get_address_space(pdev);
    peer->enabled = true;
    peer->active_xfers = 0;

    /*
     * Get peer's BAR2 scratchpad memory region for P2P transfers.
     * The peer device must be another pcie-accelerator with BAR2 initialized.
     */
    if (object_dynamic_cast(OBJECT(pdev), TYPE_PCIE_ACCEL)) {
        PCIeAccel *peer_accel = PCIE_ACCEL(pdev);
        peer->bar2 = &peer_accel->bar2;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x has BAR2 scratchpad (%lu bytes)\n",
                      bdf, (unsigned long)memory_region_size(peer->bar2));
    } else {
        peer->bar2 = NULL;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x is not a pcie-accelerator device\n",
                      bdf);
    }

    /* Add to active peer list */
    QTAILQ_INSERT_TAIL(&n->p2p.peer_list, peer, entry);
    n->p2p.num_peers++;

    trace_pcie_accel_p2p_setup(bdf);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pcie-accel: Registered P2P peer 0x%x (total: %u)\n",
                  bdf, n->p2p.num_peers);

    return 0;
}

/**
 * accel_unregister_p2p_peer - Unregister a P2P peer device
 * @n: Device state
 * @bdf: Bus:Device:Function identifier of peer
 *
 * Removes a peer device from the P2P peer list. Any ongoing transfers
 * to this peer will fail.
 */
void accel_unregister_p2p_peer(PCIeAccel *n, uint16_t bdf)
{
    AccelP2PPeer *peer = accel_find_p2p_peer(n, bdf);

    if (!peer) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x not found for unregistration\n",
                      bdf);
        return;
    }

    /* Check for active transfers */
    if (peer->active_xfers > 0) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Warning: Unregistering peer 0x%x with %u "
                      "active transfers\n", bdf, peer->active_xfers);
    }

    /* Remove from list */
    QTAILQ_REMOVE(&n->p2p.peer_list, peer, entry);
    n->p2p.num_peers--;

    /* Clear peer structure */
    memset(peer, 0, sizeof(AccelP2PPeer));

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pcie-accel: Unregistered P2P peer 0x%x (remaining: %u)\n",
                  bdf, n->p2p.num_peers);
}

/**
 * accel_get_pasid_as - Get address space for PASID
 * @n: Device state
 * @pdev: PCI device
 * @pasid: Process Address Space ID
 *
 * Returns the address space for a specific PASID. If PASID is not enabled
 * or the PASID is invalid, returns the default device address space.
 *
 * Note: Currently unused as P2P transfers use direct BAR2 memory access.
 * Kept for future PASID/SVA support.
 *
 * Returns: Address space pointer
 */
static G_GNUC_UNUSED AddressSpace *accel_get_pasid_as(PCIeAccel *n,
                                                       PCIDevice *pdev,
                                                       uint32_t pasid)
{
    if (!n->sva.enabled) {
        return pci_get_address_space(pdev);
    }

    if (pasid >= (1U << n->sva.pasid_width)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Invalid PASID %u (width=%u)\n",
                      pasid, n->sva.pasid_width);
        return pci_get_address_space(pdev);
    }

    /* Lazy initialization of PASID address space */
    if (!n->sva.pasid_as[pasid]) {
        /*
         * In a real implementation, this would query the IOMMU for
         * the PASID-specific address space. For now, use the default.
         */
        n->sva.pasid_as[pasid] = pci_get_address_space(pdev);
    }

    return n->sva.pasid_as[pasid];
}

/*
 * ===== P2P Transfer Implementation =====
 */

/**
 * accel_p2p_transfer - Perform P2P DMA transfer
 * @n: Device state
 * @req: Request structure
 * @is_write: True for write (device to peer), false for read (peer to device)
 *
 * Implements chunked P2P DMA transfer with error handling and progress tracking.
 * Large transfers are broken into chunks to avoid excessive memory allocation.
 *
 * Returns: Status code
 */
static uint16_t accel_p2p_transfer(PCIeAccel *n, AccelRequest *req, bool is_write)
{
    AccelCmd *cmd = &req->cmd;
    PCIDevice *pci = PCI_DEVICE(n);
    uint16_t peer_bdf = le32_to_cpu(cmd->dw.p2p.peer_bdf);
    uint64_t peer_addr = le64_to_cpu(cmd->dw.p2p.peer_addr);
    uint64_t host_addr = le64_to_cpu(cmd->prp1);
    uint32_t total_len = le32_to_cpu(cmd->dw.p2p.length);
    AccelP2PPeer *peer;
    uint32_t offset = 0;
    uint16_t status = ACCEL_SC_SUCCESS;
    void *bounce_buf = NULL;
    void *peer_ram;
    uint64_t bar2_size;

    /* Lookup peer device */
    peer = accel_find_p2p_peer(n, peer_bdf);
    if (!peer) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x not registered\n", peer_bdf);
        return ACCEL_SC_P2P_PEER_NOT_FOUND;
    }

    /* Check that peer has BAR2 scratchpad memory */
    if (!peer->bar2) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x has no BAR2 scratchpad\n",
                      peer_bdf);
        return ACCEL_SC_P2P_PEER_INVALID;
    }

    /* Check transfer limit */
    if (peer->active_xfers >= n->p2p.max_xfers_per_peer) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x has %u active transfers "
                      "(max=%u)\n", peer_bdf, peer->active_xfers,
                      n->p2p.max_xfers_per_peer);
        return ACCEL_SC_P2P_MAX_XFERS;
    }

    /* Get peer's BAR2 RAM pointer and validate address range */
    bar2_size = memory_region_size(peer->bar2);
    if (peer_addr + total_len > bar2_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P address 0x%" PRIx64 " + len %u exceeds "
                      "BAR2 size %" PRIu64 "\n", peer_addr, total_len, bar2_size);
        return ACCEL_SC_INVALID_PRP;
    }

    peer_ram = memory_region_get_ram_ptr(peer->bar2);
    if (!peer_ram) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x BAR2 is not RAM-backed\n",
                      peer_bdf);
        return ACCEL_SC_P2P_PEER_INVALID;
    }

    /* Mark transfer as active */
    peer->active_xfers++;

    /* Allocate bounce buffer for chunked transfer */
    uint32_t chunk_size = MIN(total_len, ACCEL_P2P_CHUNK_SIZE);
    bounce_buf = g_malloc(chunk_size);
    if (!bounce_buf) {
        peer->active_xfers--;
        return ACCEL_SC_INTERNAL_ERROR;
    }

    trace_pcie_accel_p2p_xfer(peer_bdf, peer_addr, total_len);

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pcie-accel: P2P %s START: peer=0x%x host_addr=0x%" PRIx64
                  " peer_addr=0x%" PRIx64 " len=%u bar2_size=%" PRIu64 "\n",
                  is_write ? "WRITE" : "READ", peer_bdf, host_addr,
                  peer_addr, total_len, bar2_size);

    /*
     * Transfer loop: Process data in chunks
     * For writes: Read from host -> Write to peer BAR2
     * For reads:  Read from peer BAR2 -> Write to host
     *
     * peer_addr is an offset within peer's BAR2 scratchpad memory.
     */
    while (offset < total_len && status == ACCEL_SC_SUCCESS) {
        uint32_t xfer_len = MIN(chunk_size, total_len - offset);
        MemTxResult result;

        if (is_write) {
            /* P2P Write: Host memory -> Peer BAR2 */

            /* Read from host memory (via this device's DMA) */
            result = pci_dma_read(pci, host_addr + offset, bounce_buf, xfer_len);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "pcie-accel: P2P write DMA read FAILED at 0x%"
                              PRIx64 " len %u result=%d\n",
                              host_addr + offset, xfer_len, result);
                status = ACCEL_SC_DMA_ERROR;
                break;
            }

            /* Write directly to peer's BAR2 RAM */
            memcpy((uint8_t *)peer_ram + peer_addr + offset, bounce_buf, xfer_len);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2P write chunk: host 0x%" PRIx64
                          " -> peer_ram+0x%" PRIx64 " len %u OK\n",
                          host_addr + offset, peer_addr + offset, xfer_len);

        } else {
            /* P2P Read: Peer BAR2 -> Host memory */

            /* Read directly from peer's BAR2 RAM */
            memcpy(bounce_buf, (uint8_t *)peer_ram + peer_addr + offset, xfer_len);

            /* Write to host memory (via this device's DMA) */
            result = pci_dma_write(pci, host_addr + offset, bounce_buf, xfer_len);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "pcie-accel: P2P read DMA write FAILED at 0x%"
                              PRIx64 " len %u result=%d\n",
                              host_addr + offset, xfer_len, result);
                status = ACCEL_SC_DMA_ERROR;
                break;
            }
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2P read chunk: peer_ram+0x%" PRIx64
                          " -> host 0x%" PRIx64 " len %u OK\n",
                          peer_addr + offset, host_addr + offset, xfer_len);
        }

        offset += xfer_len;
    }

    /* Cleanup */
    g_free(bounce_buf);
    peer->active_xfers--;

    /* Update statistics */
    if (status == ACCEL_SC_SUCCESS) {
        n->stats.p2p_xfers++;
        n->stats.p2p_bytes += total_len;
        req->cqe.result = cpu_to_le32(total_len);
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P %s DONE: peer=0x%x len=%u status=0x%x "
                      "total_xfers=%" PRIu64 "\n",
                      is_write ? "WRITE" : "READ", peer_bdf, total_len,
                      status, n->stats.p2p_xfers);
    } else {
        n->stats.cmd_errors++;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P %s FAILED: peer=0x%x status=0x%x "
                      "active_xfers=%u cmd_errors=%" PRIu64 "\n",
                      is_write ? "WRITE" : "READ", peer_bdf, status,
                      peer->active_xfers, n->stats.cmd_errors);
    }

    return status;
}

/**
 * accel_cmd_p2p_write - P2P write command handler
 * @n: Device state
 * @req: Request structure
 *
 * Transfers data from host memory (attached to this device) to peer device memory.
 *
 * Command parameters:
 * - prp1: Source address in host memory
 * - dw.p2p.peer_bdf: Target peer device BDF
 * - dw.p2p.peer_addr: Target address in peer device
 * - dw.p2p.length: Transfer length in bytes
 * - dw.p2p.pasid: PASID (if PASID flag set)
 *
 * Returns: Status code
 */
uint16_t accel_cmd_p2p_write(PCIeAccel *n, AccelRequest *req)
{
    return accel_p2p_transfer(n, req, true);
}

/**
 * accel_cmd_p2p_read - P2P read command handler
 * @n: Device state
 * @req: Request structure
 *
 * Transfers data from peer device memory to host memory (attached to this device).
 *
 * Command parameters:
 * - prp1: Destination address in host memory
 * - dw.p2p.peer_bdf: Source peer device BDF
 * - dw.p2p.peer_addr: Source address in peer device
 * - dw.p2p.length: Transfer length in bytes
 * - dw.p2p.pasid: PASID (if PASID flag set)
 *
 * Returns: Status code
 */
uint16_t accel_cmd_p2p_read(PCIeAccel *n, AccelRequest *req)
{
    return accel_p2p_transfer(n, req, false);
}

/*
 * ===== P2P Setup Command =====
 */

/**
 * accel_cmd_p2p_setup - P2P setup admin command
 * @n: Device state
 * @req: Request structure
 *
 * Admin command to register or unregister P2P peer devices.
 *
 * Command parameters:
 * - cdw10[15:0]:  Peer BDF (Bus:Device:Function)
 * - cdw10[16]:    Operation (0=register, 1=unregister)
 * - prp1:         Peer device information (future use)
 *
 * Returns: Status code
 */
uint16_t accel_cmd_p2p_setup(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t peer_bdf = le32_to_cpu(cmd->dw.admin.cdw10) & 0xFFFF;
    bool unregister = (le32_to_cpu(cmd->dw.admin.cdw10) >> 16) & 0x1;

    if (unregister) {
        /* Unregister peer */
        AccelP2PPeer *peer = accel_find_p2p_peer(n, peer_bdf);
        if (!peer) {
            return ACCEL_SC_P2P_PEER_NOT_FOUND;
        }

        accel_unregister_p2p_peer(n, peer_bdf);
        return ACCEL_SC_SUCCESS;

    } else {
        /* Register peer */
        PCIDevice *pdev;
        PCIBus *bus;
        int devfn;

        /*
         * In a real implementation, the host would provide information
         * about the peer device. For this implementation, we attempt to
         * find the peer device on the PCI bus by BDF.
         *
         * BDF format: [15:8] = bus, [7:3] = device, [2:0] = function
         */
        uint8_t bus_num = (peer_bdf >> 8) & 0xFF;
        uint8_t dev_num = (peer_bdf >> 3) & 0x1F;
        uint8_t func_num = peer_bdf & 0x7;
        devfn = PCI_DEVFN(dev_num, func_num);

        /* Get the PCI bus */
        bus = pci_get_bus(PCI_DEVICE(n));
        if (!bus) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Failed to get PCI bus\n");
            return ACCEL_SC_P2P_PEER_INVALID;
        }

        /* Find the peer device on the bus */
        pdev = pci_find_device(bus, bus_num, devfn);
        if (!pdev) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2P peer device 0x%x not found on bus\n",
                          peer_bdf);
            return ACCEL_SC_P2P_PEER_NOT_FOUND;
        }

        /* Register the peer */
        if (accel_register_p2p_peer(n, peer_bdf, pdev) < 0) {
            return ACCEL_SC_P2P_MAX_PEERS;
        }

        return ACCEL_SC_SUCCESS;
    }
}

/*
 * ===== P2P Statistics and Debug =====
 */

/**
 * accel_p2p_get_stats - Get P2P transfer statistics
 * @n: Device state
 * @buf: Output buffer for statistics
 * @size: Buffer size
 *
 * Returns statistics about P2P transfers for debugging and monitoring.
 * This could be exposed via a log page or debugfs interface.
 *
 * Returns: Number of bytes written
 */
size_t accel_p2p_get_stats(PCIeAccel *n, void *buf, size_t size)
{
    struct {
        uint64_t total_xfers;
        uint64_t total_bytes;
        uint32_t num_peers;
        uint32_t active_xfers;
        struct {
            uint16_t bdf;
            uint32_t active_xfers;
        } peers[ACCEL_MAX_P2P_PEERS];
    } stats;

    memset(&stats, 0, sizeof(stats));

    stats.total_xfers = n->stats.p2p_xfers;
    stats.total_bytes = n->stats.p2p_bytes;
    stats.num_peers = n->p2p.num_peers;

    /* Collect per-peer statistics */
    int peer_idx = 0;
    AccelP2PPeer *peer;
    QTAILQ_FOREACH(peer, &n->p2p.peer_list, entry) {
        if (peer_idx >= ACCEL_MAX_P2P_PEERS) {
            break;
        }

        stats.peers[peer_idx].bdf = peer->bdf;
        stats.peers[peer_idx].active_xfers = peer->active_xfers;
        stats.active_xfers += peer->active_xfers;
        peer_idx++;
    }

    /* Copy to output buffer */
    size_t copy_size = MIN(size, sizeof(stats));
    memcpy(buf, &stats, copy_size);

    return copy_size;
}

/**
 * accel_p2p_dump_state - Dump P2P state for debugging
 * @n: Device state
 *
 * Prints current P2P state to the log for debugging purposes.
 */
void accel_p2p_dump_state(PCIeAccel *n)
{
    qemu_log("PCIe Accelerator P2P State:\n");
    qemu_log("  Max peers: %u\n", n->p2p.max_peers);
    qemu_log("  Current peers: %u\n", n->p2p.num_peers);
    qemu_log("  Max xfers per peer: %u\n", n->p2p.max_xfers_per_peer);
    qemu_log("  Total transfers: %" PRIu64 "\n", n->stats.p2p_xfers);
    qemu_log("  Total bytes: %" PRIu64 "\n", n->stats.p2p_bytes);

    qemu_log("  Registered peers:\n");
    AccelP2PPeer *peer;
    QTAILQ_FOREACH(peer, &n->p2p.peer_list, entry) {
        qemu_log("    BDF: 0x%04x, Active xfers: %u\n",
                 peer->bdf, peer->active_xfers);
    }

    qemu_log("  P2P MMIO Queues (%u active):\n", n->p2p.num_p2p_queues);
    for (int i = 0; i < ACCEL_P2Q_MAX_SLOTS; i++) {
        AccelP2PQueuePair *qp = &n->p2p.p2p_queues[i];
        if (qp->active) {
            qemu_log("    Slot %d: peer=0x%04x isq(h=%u t=%u) rcq(h=%u t=%u) "
                     "out(sq_t=%u cq_t=%u slot=%u)\n",
                     i, qp->peer_bdf,
                     qp->isq.head, qp->isq.tail,
                     qp->rcq.head, qp->rcq.tail,
                     qp->outbound.sq_tail, qp->outbound.cq_tail,
                     qp->outbound.our_slot);
        }
    }
}

/*
 * ==========================================================================
 * P2P MMIO Queue Implementation
 *
 * NVMe-style queue pairs for direct device-to-device communication.
 * Each device exposes per-peer SQ/CQ in its BAR2 (RAM) with doorbells
 * in BAR0 (MMIO). Cross-device writes use address_space_write().
 * ==========================================================================
 */

/**
 * accel_p2q_write_cqe_to_peer - Write a CQE to a peer's receive CQ
 * @n: This device state
 * @qp: Queue pair for the peer
 * @cqe: Completion queue entry to write
 *
 * Writes a CQE to the peer's BAR2 receive CQ region and rings the
 * peer's CQ notify doorbell via cross-device MMIO writes.
 *
 * Returns: ACCEL_SC_SUCCESS on success, error code on failure
 */
static uint16_t accel_p2q_write_cqe_to_peer(PCIeAccel *n,
                                              AccelP2PQueuePair *qp,
                                              AccelCqe *cqe)
{
    MemTxResult result;
    hwaddr cqe_addr;
    hwaddr db_addr;
    uint32_t db_val;

    /* Calculate CQE address in peer's BAR2 receive CQ */
    cqe_addr = qp->outbound.peer_bar2 +
               ACCEL_P2Q_CQ_OFFSET(qp->outbound.our_slot) +
               (qp->outbound.cq_tail << ACCEL_CQES);

    /* Set phase bit in CQE status */
    cqe->status = cpu_to_le16(
        (le16_to_cpu(cqe->status) & ACCEL_CQE_STATUS_CODE_MASK) |
        (qp->outbound.cq_phase & ACCEL_CQE_STATUS_PHASE_MASK));

    /* Write CQE to peer's BAR2 */
    result = address_space_write(qp->outbound.peer_as, cqe_addr,
                                 MEMTXATTRS_UNSPECIFIED,
                                 cqe, sizeof(*cqe));
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q failed to write CQE to peer 0x%x "
                      "at 0x%" PRIx64 "\n", qp->peer_bdf, cqe_addr);
        return ACCEL_SC_P2Q_XFER_ERROR;
    }

    /* Advance CQ tail with phase toggle on wrap */
    qp->outbound.cq_tail++;
    if (qp->outbound.cq_tail >= ACCEL_P2Q_CQ_ENTRIES) {
        qp->outbound.cq_tail = 0;
        qp->outbound.cq_phase = !qp->outbound.cq_phase;
    }

    /* Ring peer's CQ notify doorbell */
    db_addr = qp->outbound.peer_bar0 +
              ACCEL_P2Q_CQ_NOTIFY_DB(qp->outbound.our_slot);
    db_val = cpu_to_le32(qp->outbound.cq_tail);

    result = address_space_write(qp->outbound.peer_as, db_addr,
                                 MEMTXATTRS_UNSPECIFIED,
                                 &db_val, sizeof(db_val));
    if (result != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q failed to ring CQ doorbell on peer 0x%x "
                      "at 0x%" PRIx64 "\n", qp->peer_bdf, db_addr);
    }

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q wrote CQE to peer 0x%x: cqe_addr=0x%" PRIx64
                  " cq_tail=%u phase=%u\n",
                  qp->peer_bdf, cqe_addr, qp->outbound.cq_tail,
                  qp->outbound.cq_phase);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_p2q_cmd_mmio_write - Handle P2P MMIO write command
 * @n: Target device state (this device)
 * @qp: Queue pair identifying the submitter
 * @cmd: Command from inbound SQ
 *
 * Transfers data FROM submitter's BAR2 data region TO this device's BAR2
 * data region.
 *
 * Returns: Status code
 */
static uint16_t accel_p2q_cmd_mmio_write(PCIeAccel *n,
                                          AccelP2PQueuePair *qp,
                                          AccelCmd *cmd)
{
    uint64_t src_off = le64_to_cpu(cmd->prp1);
    uint64_t dst_off = le64_to_cpu(cmd->dw.p2p.peer_addr);
    uint32_t length = le32_to_cpu(cmd->dw.p2p.length);
    void *local_bar2;
    void *bounce_buf;
    uint32_t data_region_size;
    MemTxResult result;

    data_region_size = ACCEL_BAR2_SIZE - ACCEL_P2Q_DATA_OFFSET;

    /* Validate source offset (in submitter's BAR2 data region) */
    if (src_off + length > data_region_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q MMIO_WRITE src 0x%" PRIx64
                      " + len %u exceeds data region %u\n",
                      src_off, length, data_region_size);
        return ACCEL_SC_P2Q_DATA_RANGE;
    }

    /* Validate destination offset (in our BAR2 data region) */
    if (dst_off + length > data_region_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q MMIO_WRITE dst 0x%" PRIx64
                      " + len %u exceeds data region %u\n",
                      dst_off, length, data_region_size);
        return ACCEL_SC_P2Q_DATA_RANGE;
    }

    if (length == 0 || length > data_region_size) {
        return ACCEL_SC_P2P_LEN_INVALID;
    }

    /* Allocate bounce buffer */
    bounce_buf = g_malloc(MIN(length, ACCEL_P2P_CHUNK_SIZE));

    /* Read data from submitter's BAR2 data region via cross-device MMIO */
    hwaddr src_addr = qp->outbound.peer_bar2 + ACCEL_P2Q_DATA_OFFSET + src_off;
    local_bar2 = memory_region_get_ram_ptr(&n->bar2);

    uint32_t offset = 0;
    while (offset < length) {
        uint32_t chunk = MIN(ACCEL_P2P_CHUNK_SIZE, length - offset);

        result = address_space_read(qp->outbound.peer_as,
                                     src_addr + offset,
                                     MEMTXATTRS_UNSPECIFIED,
                                     bounce_buf, chunk);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2Q MMIO_WRITE read from peer failed "
                          "at 0x%" PRIx64 "\n", src_addr + offset);
            g_free(bounce_buf);
            return ACCEL_SC_P2Q_XFER_ERROR;
        }

        /* Write to local BAR2 data region */
        memcpy((uint8_t *)local_bar2 + ACCEL_P2Q_DATA_OFFSET + dst_off + offset,
               bounce_buf, chunk);
        offset += chunk;
    }

    g_free(bounce_buf);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q MMIO_WRITE: peer=0x%x src=0x%" PRIx64
                  " dst=0x%" PRIx64 " len=%u OK\n",
                  qp->peer_bdf, src_off, dst_off, length);

    n->stats.p2p_xfers++;
    n->stats.p2p_bytes += length;

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_p2q_cmd_mmio_read - Handle P2P MMIO read command
 * @n: Target device state (this device)
 * @qp: Queue pair identifying the submitter
 * @cmd: Command from inbound SQ
 *
 * Transfers data FROM this device's BAR2 data region TO submitter's BAR2
 * data region.
 *
 * Returns: Status code
 */
static uint16_t accel_p2q_cmd_mmio_read(PCIeAccel *n,
                                         AccelP2PQueuePair *qp,
                                         AccelCmd *cmd)
{
    uint64_t dst_off = le64_to_cpu(cmd->prp1);  /* dest in submitter's data region */
    uint64_t src_off = le64_to_cpu(cmd->dw.p2p.peer_addr);  /* src in our data region */
    uint32_t length = le32_to_cpu(cmd->dw.p2p.length);
    void *local_bar2;
    void *bounce_buf;
    uint32_t data_region_size;
    MemTxResult result;

    data_region_size = ACCEL_BAR2_SIZE - ACCEL_P2Q_DATA_OFFSET;

    /* Validate source offset (in our BAR2 data region) */
    if (src_off + length > data_region_size) {
        return ACCEL_SC_P2Q_DATA_RANGE;
    }

    /* Validate destination offset (in submitter's BAR2 data region) */
    if (dst_off + length > data_region_size) {
        return ACCEL_SC_P2Q_DATA_RANGE;
    }

    if (length == 0 || length > data_region_size) {
        return ACCEL_SC_P2P_LEN_INVALID;
    }

    bounce_buf = g_malloc(MIN(length, ACCEL_P2P_CHUNK_SIZE));
    local_bar2 = memory_region_get_ram_ptr(&n->bar2);

    hwaddr dst_addr = qp->outbound.peer_bar2 + ACCEL_P2Q_DATA_OFFSET + dst_off;

    uint32_t offset = 0;
    while (offset < length) {
        uint32_t chunk = MIN(ACCEL_P2P_CHUNK_SIZE, length - offset);

        /* Read from local BAR2 data region */
        memcpy(bounce_buf,
               (uint8_t *)local_bar2 + ACCEL_P2Q_DATA_OFFSET + src_off + offset,
               chunk);

        /* Write to submitter's BAR2 data region via cross-device MMIO */
        result = address_space_write(qp->outbound.peer_as,
                                      dst_addr + offset,
                                      MEMTXATTRS_UNSPECIFIED,
                                      bounce_buf, chunk);
        if (result != MEMTX_OK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2Q MMIO_READ write to peer failed "
                          "at 0x%" PRIx64 "\n", dst_addr + offset);
            g_free(bounce_buf);
            return ACCEL_SC_P2Q_XFER_ERROR;
        }

        offset += chunk;
    }

    g_free(bounce_buf);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q MMIO_READ: peer=0x%x src=0x%" PRIx64
                  " dst=0x%" PRIx64 " len=%u OK\n",
                  qp->peer_bdf, src_off, dst_off, length);

    n->stats.p2p_xfers++;
    n->stats.p2p_bytes += length;

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_p2q_cmd_loopback - Handle P2P loopback test command
 * @n: Target device state (this device)
 * @cmd: Command from inbound SQ
 *
 * Reads data from local BAR2 data region, optionally XORs with pattern,
 * and writes back. Used for testing P2P queue infrastructure.
 *
 * Returns: Status code
 */
static uint16_t accel_p2q_cmd_loopback(PCIeAccel *n, AccelCmd *cmd)
{
    uint64_t data_off = le64_to_cpu(cmd->dw.p2p.peer_addr);
    uint32_t length = le32_to_cpu(cmd->dw.p2p.length);
    uint32_t pattern = le32_to_cpu(cmd->dw.loopback.pattern);
    void *local_bar2;
    uint32_t data_region_size;

    data_region_size = ACCEL_BAR2_SIZE - ACCEL_P2Q_DATA_OFFSET;

    if (data_off + length > data_region_size || length == 0) {
        return ACCEL_SC_P2Q_DATA_RANGE;
    }

    local_bar2 = memory_region_get_ram_ptr(&n->bar2);
    uint8_t *data = (uint8_t *)local_bar2 + ACCEL_P2Q_DATA_OFFSET + data_off;

    /* XOR with pattern if specified */
    if (pattern != 0) {
        uint32_t *data32 = (uint32_t *)data;
        for (uint32_t i = 0; i < length / 4; i++) {
            data32[i] ^= pattern;
        }
    }

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q LOOPBACK: off=0x%" PRIx64 " len=%u OK\n",
                  data_off, length);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_process_p2p_sq - Process inbound P2P submission queue
 * @opaque: AccelP2PQueuePair pointer
 *
 * Bottom-half handler triggered by P2P SQ tail doorbell writes from peers.
 * Reads commands from BAR2 inbound SQ, processes them, and writes CQEs
 * back to the submitting peer's receive CQ via cross-device MMIO.
 */
void accel_process_p2p_sq(void *opaque)
{
    AccelP2PQueuePair *qp = opaque;
    PCIeAccel *n = qp->ctrl;
    void *bar2_ram;
    AccelCmd cmd;
    AccelCqe cqe;
    uint16_t status;

    if (!qp->active) {
        return;
    }

    bar2_ram = memory_region_get_ram_ptr(&n->bar2);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q process_sq: slot=%u peer=0x%x "
                  "head=%u tail=%u\n",
                  qp->slot, qp->peer_bdf, qp->isq.head, qp->isq.tail);

    /* Process commands until SQ is empty */
    while (qp->isq.head != qp->isq.tail) {
        /* Read command from BAR2 inbound SQ (local RAM access) */
        uint32_t sq_offset = ACCEL_P2Q_SQ_OFFSET(qp->slot) +
                             (qp->isq.head << ACCEL_SQES);
        memcpy(&cmd, (uint8_t *)bar2_ram + sq_offset, sizeof(cmd));

        /* Advance SQ head */
        qp->isq.head = (qp->isq.head + 1) % qp->isq.size;

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: P2Q cmd: slot=%u opcode=0x%x cid=%u "
                      "from peer 0x%x\n",
                      qp->slot, cmd.opcode, le16_to_cpu(cmd.cid),
                      qp->peer_bdf);

        /* Dispatch command */
        switch (cmd.opcode) {
        case ACCEL_P2Q_CMD_MMIO_WRITE:
            status = accel_p2q_cmd_mmio_write(n, qp, &cmd);
            break;

        case ACCEL_P2Q_CMD_MMIO_READ:
            status = accel_p2q_cmd_mmio_read(n, qp, &cmd);
            break;

        case ACCEL_P2Q_CMD_LOOPBACK:
            status = accel_p2q_cmd_loopback(n, &cmd);
            break;

        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2Q invalid opcode 0x%x from peer 0x%x\n",
                          cmd.opcode, qp->peer_bdf);
            status = ACCEL_SC_INVALID_OPCODE;
            break;
        }

        /* Build CQE */
        memset(&cqe, 0, sizeof(cqe));
        cqe.cid = cmd.cid;
        cqe.sq_id = cpu_to_le16(qp->slot);
        cqe.sq_head = cpu_to_le16(qp->isq.head);
        cqe.result = (status == ACCEL_SC_SUCCESS) ?
                     cmd.dw.p2p.length : 0;
        cqe.status = cpu_to_le16(
            (status << ACCEL_CQE_STATUS_CODE_SHIFT) & ACCEL_CQE_STATUS_CODE_MASK);

        /* Write CQE to peer's receive CQ via cross-device MMIO */
        accel_p2q_write_cqe_to_peer(n, qp, &cqe);

        n->stats.cmd_processed++;
        if (status == ACCEL_SC_SUCCESS) {
            n->stats.cmd_completed++;
        } else {
            n->stats.cmd_errors++;
        }
    }
}

/**
 * accel_cmd_p2p_queue_setup - Admin command: Set up a P2P MMIO queue pair
 * @n: Device state
 * @req: Request structure
 *
 * Sets up a P2P queue pair with a peer device. The host provides peer
 * BAR addresses and slot assignments via admin command parameters.
 *
 * CDW10[15:0]:  Peer BDF
 * CDW10[19:16]: Slot number for this peer (0-6)
 * CDW10[23:20]: Our slot in peer's device (0-6)
 * CDW11:        Peer BAR0 address (low 32)
 * CDW12:        Peer BAR0 address (high 32)
 * CDW13:        Peer BAR2 address (low 32)
 * CDW14:        Peer BAR2 address (high 32)
 *
 * Returns: Status code
 */
uint16_t accel_cmd_p2p_queue_setup(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint32_t cdw10 = le32_to_cpu(cmd->dw.admin.cdw10);
    uint16_t peer_bdf = cdw10 & 0xFFFF;
    uint8_t slot = (cdw10 >> 16) & 0xF;
    uint8_t our_slot = (cdw10 >> 20) & 0xF;
    hwaddr peer_bar0 = ((hwaddr)le32_to_cpu(cmd->dw.admin.cdw12) << 32) |
                       le32_to_cpu(cmd->dw.admin.cdw11);
    hwaddr peer_bar2 = ((hwaddr)le32_to_cpu(cmd->dw.admin.cdw14) << 32) |
                       le32_to_cpu(cmd->dw.admin.cdw13);
    AccelP2PQueuePair *qp;
    PCIDevice *pdev;
    PCIBus *bus;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q_SETUP: peer=0x%x slot=%u our_slot=%u "
                  "bar0=0x%" PRIx64 " bar2=0x%" PRIx64 "\n",
                  peer_bdf, slot, our_slot, peer_bar0, peer_bar2);

    /* Validate slot number */
    if (slot >= ACCEL_P2Q_MAX_SLOTS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q_SETUP invalid slot %u (max=%u)\n",
                      slot, ACCEL_P2Q_MAX_SLOTS - 1);
        return ACCEL_SC_P2Q_INVALID_SLOT;
    }

    if (our_slot >= ACCEL_P2Q_MAX_SLOTS) {
        return ACCEL_SC_P2Q_INVALID_SLOT;
    }

    qp = &n->p2p.p2p_queues[slot];

    /* Check if slot is already active */
    if (qp->active) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q_SETUP slot %u already active\n", slot);
        return ACCEL_SC_P2Q_SLOT_ACTIVE;
    }

    /* Find the peer device on the PCI bus */
    bus = pci_get_bus(PCI_DEVICE(n));
    if (!bus) {
        return ACCEL_SC_P2P_PEER_INVALID;
    }

    uint8_t bus_num = (peer_bdf >> 8) & 0xFF;
    uint8_t dev_num = (peer_bdf >> 3) & 0x1F;
    uint8_t func_num = peer_bdf & 0x7;
    pdev = pci_find_device(bus, bus_num, PCI_DEVFN(dev_num, func_num));

    if (!pdev) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q_SETUP peer 0x%x not found\n", peer_bdf);
        return ACCEL_SC_P2P_PEER_NOT_FOUND;
    }

    /* Verify peer is a pcie-accelerator device */
    if (!object_dynamic_cast(OBJECT(pdev), TYPE_PCIE_ACCEL)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q_SETUP peer 0x%x is not a "
                      "pcie-accelerator\n", peer_bdf);
        return ACCEL_SC_P2Q_PEER_MISMATCH;
    }

    /* Initialize queue pair */
    memset(qp, 0, sizeof(*qp));
    qp->ctrl = n;
    qp->slot = slot;
    qp->peer_bdf = peer_bdf;
    qp->active = true;

    /* Initialize inbound SQ state */
    qp->isq.head = 0;
    qp->isq.tail = 0;
    qp->isq.size = ACCEL_P2Q_SQ_ENTRIES;

    /* Initialize receive CQ state */
    qp->rcq.head = 0;
    qp->rcq.tail = 0;
    qp->rcq.size = ACCEL_P2Q_CQ_ENTRIES;
    qp->rcq.phase = 1;  /* Expect phase=1 initially */

    /* Initialize outbound tracking */
    qp->outbound.sq_tail = 0;
    qp->outbound.cq_tail = 0;
    qp->outbound.cq_phase = 1;  /* Start with phase=1 */
    qp->outbound.our_slot = our_slot;
    qp->outbound.peer_bar0 = peer_bar0;
    qp->outbound.peer_bar2 = peer_bar2;
    qp->outbound.peer_as = pci_get_address_space(pdev);
    qp->outbound.peer_dev = pdev;

    /* Clear BAR2 SQ and CQ regions for this slot */
    void *bar2_ram = memory_region_get_ram_ptr(&n->bar2);
    memset((uint8_t *)bar2_ram + ACCEL_P2Q_SQ_OFFSET(slot), 0,
           ACCEL_P2Q_SQ_SIZE);
    memset((uint8_t *)bar2_ram + ACCEL_P2Q_CQ_OFFSET(slot), 0,
           ACCEL_P2Q_CQ_SIZE);

    /* Create BH for inbound SQ processing */
    qp->sq_bh = qemu_bh_new(accel_process_p2p_sq, qp);

    n->p2p.num_p2p_queues++;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q_SETUP SUCCESS: slot=%u peer=0x%x "
                  "our_slot=%u total=%u\n",
                  slot, peer_bdf, our_slot, n->p2p.num_p2p_queues);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_cmd_p2p_queue_teardown - Admin command: Tear down a P2P queue pair
 * @n: Device state
 * @req: Request structure
 *
 * CDW10[3:0]: Slot number to tear down
 *
 * Returns: Status code
 */
uint16_t accel_cmd_p2p_queue_teardown(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint8_t slot = le32_to_cpu(cmd->dw.admin.cdw10) & 0xF;
    AccelP2PQueuePair *qp;

    if (slot >= ACCEL_P2Q_MAX_SLOTS) {
        return ACCEL_SC_P2Q_INVALID_SLOT;
    }

    qp = &n->p2p.p2p_queues[slot];
    if (!qp->active) {
        return ACCEL_SC_P2Q_INVALID_SLOT;
    }

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2Q_TEARDOWN: slot=%u peer=0x%x\n",
                  slot, qp->peer_bdf);

    /* Cancel and free BH */
    if (qp->sq_bh) {
        qemu_bh_cancel(qp->sq_bh);
        qemu_bh_delete(qp->sq_bh);
        qp->sq_bh = NULL;
    }

    qp->active = false;
    n->p2p.num_p2p_queues--;

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_p2p_queue_doorbell - Handle P2P queue doorbell writes
 * @n: Device state
 * @offset: Offset within P2P doorbell region (relative to 0x4000)
 * @val: Doorbell value
 *
 * Called from accel_mmio_write() when a peer device writes to our
 * P2P doorbell registers via cross-device MMIO.
 */
void accel_p2p_queue_doorbell(PCIeAccel *n, hwaddr offset, uint32_t val)
{
    uint32_t slot = offset / ACCEL_P2Q_DB_STRIDE;
    bool is_cq = (offset % ACCEL_P2Q_DB_STRIDE) >= 4;
    uint16_t new_val = val & 0xFFFF;

    if (slot >= ACCEL_P2Q_MAX_SLOTS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q doorbell invalid slot %u\n", slot);
        return;
    }

    AccelP2PQueuePair *qp = &n->p2p.p2p_queues[slot];
    if (!qp->active) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2Q doorbell for inactive slot %u\n", slot);
        return;
    }

    if (is_cq) {
        /* CQ Notify Doorbell - peer wrote a CQE to our receive CQ */
        if (new_val >= qp->rcq.size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2Q CQ doorbell value %u >= size %u\n",
                          new_val, qp->rcq.size);
            return;
        }

        qp->rcq.tail = new_val;

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: P2Q CQ notify: slot=%u new_tail=%u "
                      "(head=%u)\n", slot, new_val, qp->rcq.head);

        /*
         * TODO: Process received CQEs. For now, the host/driver polls
         * the receive CQ region in BAR2. In the future, we could fire
         * an MSI-X interrupt or schedule a BH to notify the driver.
         */
    } else {
        /* SQ Tail Doorbell - peer submitted commands to our inbound SQ */
        if (new_val >= qp->isq.size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2Q SQ doorbell value %u >= size %u\n",
                          new_val, qp->isq.size);
            return;
        }

        qp->isq.tail = new_val;

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: P2Q SQ doorbell: slot=%u new_tail=%u "
                      "(head=%u)\n", slot, new_val, qp->isq.head);

        /* Schedule command processing */
        qemu_bh_schedule(qp->sq_bh);
    }
}

/**
 * accel_p2p_queue_reset - Reset all P2P queue pairs
 * @n: Device state
 *
 * Called during device reset. Cancels all BHs and clears state.
 */
void accel_p2p_queue_reset(PCIeAccel *n)
{
    for (int i = 0; i < ACCEL_P2Q_MAX_SLOTS; i++) {
        AccelP2PQueuePair *qp = &n->p2p.p2p_queues[i];

        if (qp->sq_bh) {
            qemu_bh_cancel(qp->sq_bh);
            qemu_bh_delete(qp->sq_bh);
            qp->sq_bh = NULL;
        }

        memset(qp, 0, sizeof(*qp));
    }

    n->p2p.num_p2p_queues = 0;
}

/**
 * accel_p2p_queue_cleanup - Cleanup P2P queue resources on device exit
 * @n: Device state
 */
void accel_p2p_queue_cleanup(PCIeAccel *n)
{
    accel_p2p_queue_reset(n);
}
