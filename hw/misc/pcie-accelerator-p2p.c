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

    /* Check if already registered */
    if (accel_find_p2p_peer(n, bdf)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x already registered\n", bdf);
        return -1;
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
                  "pcie-accel: P2P %s: peer=0x%x host_addr=0x%" PRIx64
                  " peer_addr=0x%" PRIx64 " len=%u\n",
                  is_write ? "WRITE" : "READ", peer_bdf, host_addr,
                  peer_addr, total_len);

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
                              "pcie-accel: P2P write failed reading host at 0x%"
                              PRIx64 " len %u\n", host_addr + offset, xfer_len);
                status = ACCEL_SC_DMA_ERROR;
                break;
            }

            /* Write directly to peer's BAR2 RAM */
            memcpy((uint8_t *)peer_ram + peer_addr + offset, bounce_buf, xfer_len);

        } else {
            /* P2P Read: Peer BAR2 -> Host memory */

            /* Read directly from peer's BAR2 RAM */
            memcpy(bounce_buf, (uint8_t *)peer_ram + peer_addr + offset, xfer_len);

            /* Write to host memory (via this device's DMA) */
            result = pci_dma_write(pci, host_addr + offset, bounce_buf, xfer_len);
            if (result != MEMTX_OK) {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "pcie-accel: P2P read failed writing host at 0x%"
                              PRIx64 " len %u\n", host_addr + offset, xfer_len);
                status = ACCEL_SC_DMA_ERROR;
                break;
            }
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
    } else {
        n->stats.cmd_errors++;
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
}
