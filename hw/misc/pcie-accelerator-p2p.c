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
     * Get peer's CMB memory region for P2P transfers.
     * The peer device must be another pcie-accelerator with CMB initialized.
     */
    if (object_dynamic_cast(OBJECT(pdev), TYPE_PCIE_ACCEL)) {
        PCIeAccel *peer_accel = PCIE_ACCEL(pdev);
        peer->cmb = &peer_accel->cmb;
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x has CMB (%lu bytes)\n",
                      bdf, (unsigned long)memory_region_size(peer->cmb));
    } else {
        peer->cmb = NULL;
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
 * Note: Currently unused as P2P transfers use direct CMB memory access.
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
    uint64_t host_addr = le64_to_cpu(cmd->dbd.prpl.prp1);
    uint32_t total_len = le32_to_cpu(cmd->dw.p2p.length);
    AccelP2PPeer *peer;
    uint32_t offset = 0;
    uint16_t status = ACCEL_SC_SUCCESS;
    void *bounce_buf = NULL;
    void *peer_ram;
    uint64_t cmb_size;

    /* Lookup peer device */
    peer = accel_find_p2p_peer(n, peer_bdf);
    if (!peer) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x not registered\n", peer_bdf);
        return ACCEL_SC_P2P_PEER_NOT_FOUND;
    }

    /* Check that peer has CMB memory */
    if (!peer->cmb) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x has no CMB\n",
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

    /* Get peer's CMB RAM pointer and validate address range */
    cmb_size = memory_region_size(peer->cmb);
    if (peer_addr + total_len > cmb_size) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P address 0x%" PRIx64 " + len %u exceeds "
                      "CMB size %" PRIu64 "\n", peer_addr, total_len, cmb_size);
        return ACCEL_SC_INVALID_PRP;
    }

    peer_ram = memory_region_get_ram_ptr(peer->cmb);
    if (!peer_ram) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P peer 0x%x CMB is not RAM-backed\n",
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
                  " peer_addr=0x%" PRIx64 " len=%u cmb_size=%" PRIu64 "\n",
                  is_write ? "WRITE" : "READ", peer_bdf, host_addr,
                  peer_addr, total_len, cmb_size);

    /*
     * Transfer loop: Process data in chunks
     * For writes: Read from host -> Write to peer CMB
     * For reads:  Read from peer CMB -> Write to host
     *
     * peer_addr is an offset within peer's CMB memory.
     */
    while (offset < total_len && status == ACCEL_SC_SUCCESS) {
        uint32_t xfer_len = MIN(chunk_size, total_len - offset);
        MemTxResult result;

        if (is_write) {
            /* P2P Write: Host memory -> Peer CMB */

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

            /* Write directly to peer's CMB RAM */
            memcpy((uint8_t *)peer_ram + peer_addr + offset, bounce_buf, xfer_len);
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2P write chunk: host 0x%" PRIx64
                          " -> peer_ram+0x%" PRIx64 " len %u OK\n",
                          host_addr + offset, peer_addr + offset, xfer_len);

        } else {
            /* P2P Read: Peer CMB -> Host memory */

            /* Read directly from peer's CMB RAM */
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
        req->cqe.result = cpu_to_le64(total_len);
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
 * ===== P2P Setup / Teardown Commands =====
 */

/**
 * accel_cmd_p2p_setup - P2P setup admin command (opcode 0x0F)
 * @n: Device state
 * @req: Request structure
 *
 * Registers a P2P peer device AND sets up a ring buffer slot in one command.
 *
 * Command fields (from cmd->p2p_setup):
 * - peer_bdf:   Peer Bus:Device:Function
 * - slot:       Ring slot in our device (0-6)
 * - peer_slot:  Our slot in peer's device (0-6)
 * - peer_bar0:  Peer's BAR0 physical address
 *
 * Returns: Status code
 */
uint16_t accel_cmd_p2p_setup(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t peer_bdf = le16_to_cpu(cmd->p2p_setup.peer_bdf);
    uint8_t slot = cmd->p2p_setup.slot;
    uint8_t peer_slot = cmd->p2p_setup.peer_slot;
    hwaddr peer_bar0 = le64_to_cpu(cmd->p2p_setup.peer_bar0);
    AccelP2PRing *ring;
    PCIDevice *pdev;
    PCIBus *bus;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2P_SETUP: peer=0x%x slot=%u peer_slot=%u "
                  "bar0=0x%" PRIx64 "\n",
                  peer_bdf, slot, peer_slot, peer_bar0);

    /* Validate slot numbers */
    if (slot >= ACCEL_P2R_MAX_SLOTS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P_SETUP invalid slot %u (max=%u)\n",
                      slot, ACCEL_P2R_MAX_SLOTS - 1);
        return ACCEL_SC_P2R_INVALID_SLOT;
    }

    if (peer_slot >= ACCEL_P2R_MAX_SLOTS) {
        return ACCEL_SC_P2R_INVALID_SLOT;
    }

    ring = &n->p2p.p2p_rings[slot];

    /* Check if slot is already active */
    if (ring->active) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P_SETUP slot %u already active\n", slot);
        return ACCEL_SC_P2R_SLOT_ACTIVE;
    }

    /* Find the peer device on the PCI bus */
    uint8_t bus_num = (peer_bdf >> 8) & 0xFF;
    uint8_t dev_num = (peer_bdf >> 3) & 0x1F;
    uint8_t func_num = peer_bdf & 0x7;

    bus = pci_get_bus(PCI_DEVICE(n));
    if (!bus) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Failed to get PCI bus\n");
        return ACCEL_SC_P2P_PEER_INVALID;
    }

    pdev = pci_find_device(bus, bus_num, PCI_DEVFN(dev_num, func_num));
    if (!pdev) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P_SETUP peer 0x%x not found\n", peer_bdf);
        return ACCEL_SC_P2P_PEER_NOT_FOUND;
    }

    /* Verify peer is a pcie-accelerator device */
    if (!object_dynamic_cast(OBJECT(pdev), TYPE_PCIE_ACCEL)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2P_SETUP peer 0x%x is not a "
                      "pcie-accelerator\n", peer_bdf);
        return ACCEL_SC_P2R_PEER_MISMATCH;
    }

    /* Register the peer device (idempotent if already registered) */
    if (accel_register_p2p_peer(n, peer_bdf, pdev) < 0) {
        return ACCEL_SC_P2P_MAX_PEERS;
    }

    /* Initialize ring buffer */
    memset(ring, 0, sizeof(*ring));
    ring->ctrl = n;
    ring->slot = slot;
    ring->peer_bdf = peer_bdf;
    ring->active = true;

    /* Initialize inbound ring state */
    ring->head = 0;
    ring->tail = 0;
    ring->size = ACCEL_RING_DATA_SIZE;

    /* Initialize outbound tracking */
    ring->outbound.our_slot = peer_slot;
    ring->outbound.peer_bar0 = peer_bar0;
    ring->outbound.peer_as = pci_get_address_space(pdev);
    ring->outbound.peer_dev = pdev;

    /* Clear ring region in CMB: header + data area */
    void *cmb_ram = memory_region_get_ram_ptr(&n->cmb);
    uint8_t *ring_base = (uint8_t *)cmb_ram + ACCEL_RING_OFFSET(slot);
    memset(ring_base, 0, ACCEL_RING_SIZE);

    /* Initialize ring header */
    AccelRingHdr *hdr = (AccelRingHdr *)ring_base;
    hdr->head = 0;
    hdr->tail = 0;
    hdr->size = cpu_to_le32(ACCEL_RING_DATA_SIZE);

    /* Create BH for inbound ring processing */
    ring->bh = qemu_bh_new(accel_process_p2p_ring, ring);

    n->p2p.num_p2p_rings++;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2P_SETUP SUCCESS: slot=%u peer=0x%x "
                  "peer_slot=%u total=%u\n",
                  slot, peer_bdf, peer_slot, n->p2p.num_p2p_rings);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_cmd_p2p_teardown - P2P teardown admin command (opcode 0x10)
 * @n: Device state
 * @req: Request structure
 *
 * Tears down a ring buffer slot AND unregisters the peer device in one command.
 *
 * Command fields (from cmd->p2p_teardown):
 * - peer_bdf:  Peer BDF to unregister
 * - slot:      Ring slot to tear down (0-6)
 *
 * Returns: Status code
 */
uint16_t accel_cmd_p2p_teardown(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t peer_bdf = le16_to_cpu(cmd->p2p_teardown.peer_bdf);
    uint8_t slot = cmd->p2p_teardown.slot;
    AccelP2PRing *ring;

    if (slot >= ACCEL_P2R_MAX_SLOTS) {
        return ACCEL_SC_P2R_INVALID_SLOT;
    }

    ring = &n->p2p.p2p_rings[slot];
    if (!ring->active) {
        return ACCEL_SC_P2R_INVALID_SLOT;
    }

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2P_TEARDOWN: slot=%u peer=0x%x\n",
                  slot, ring->peer_bdf);

    /* Cancel and free BH */
    if (ring->bh) {
        qemu_bh_cancel(ring->bh);
        qemu_bh_delete(ring->bh);
        ring->bh = NULL;
    }

    ring->active = false;
    n->p2p.num_p2p_rings--;

    /* Unregister the peer device */
    accel_unregister_p2p_peer(n, peer_bdf);

    return ACCEL_SC_SUCCESS;
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

    qemu_log("  P2P Ring Buffers (%u active):\n", n->p2p.num_p2p_rings);
    for (int i = 0; i < ACCEL_P2R_MAX_SLOTS; i++) {
        AccelP2PRing *ring = &n->p2p.p2p_rings[i];
        if (ring->active) {
            qemu_log("    Slot %d: peer=0x%04x head=%u tail=%u size=%u "
                     "out(slot=%u bar0=0x%" PRIx64 ")\n",
                     i, ring->peer_bdf,
                     ring->head, ring->tail, ring->size,
                     ring->outbound.our_slot, ring->outbound.peer_bar0);
        }
    }
}

/*
 * ==========================================================================
 * P2P Ring Buffer Implementation
 *
 * Unidirectional ring buffers for direct device-to-device communication.
 * Each device exposes per-peer inbound rings in its BAR2 CMB (RAM) with
 * doorbells in BAR0 MMIO. Cross-device writes use address_space_write().
 *
 * Data flow:
 *   1. Device A writes message(s) to Device B's inbound ring via
 *      address_space_write()
 *   2. Device A writes new tail to B's ring header
 *   3. Device A rings B's tail doorbell -> B's BH triggered
 *   4. B's BH consumes messages from [head..tail), advances head
 * ==========================================================================
 */

/**
 * accel_process_p2p_ring - Process inbound P2P ring buffer messages
 * @opaque: AccelP2PRing pointer
 *
 * Bottom-half handler triggered by P2P ring tail doorbell writes from peers.
 * Reads messages from CMB inbound ring and processes them by type.
 */
void accel_process_p2p_ring(void *opaque)
{
    AccelP2PRing *ring = opaque;
    PCIeAccel *n = ring->ctrl;
    void *cmb_ram;
    uint8_t *ring_base;
    AccelRingHdr *hdr;

    if (!ring->active) {
        return;
    }

    cmb_ram = memory_region_get_ram_ptr(&n->cmb);
    ring_base = (uint8_t *)cmb_ram + ACCEL_RING_OFFSET(ring->slot);
    hdr = (AccelRingHdr *)ring_base;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: P2R process_ring: slot=%u peer=0x%x "
                  "head=%u tail=%u\n",
                  ring->slot, ring->peer_bdf, ring->head, ring->tail);

    /* Process messages from [head..tail) in the circular data area */
    uint8_t *data_area = ring_base + ACCEL_RING_DATA_OFFSET;
    uint32_t data_size = ring->size;

    while (ring->head != ring->tail) {
        AccelRingMsg *msg;
        uint32_t msg_len;
        uint32_t avail;

        /* Calculate available bytes (handling wrap) */
        if (ring->tail >= ring->head) {
            avail = ring->tail - ring->head;
        } else {
            avail = data_size - ring->head + ring->tail;
        }

        if (avail < ACCEL_RING_MSG_HDR_SIZE) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2R ring %u: insufficient data for "
                          "message header (avail=%u)\n", ring->slot, avail);
            break;
        }

        /* Read message header at current head position */
        msg = (AccelRingMsg *)(data_area + ring->head);
        msg_len = le32_to_cpu(msg->length);

        /* Validate message length */
        if (msg_len < ACCEL_RING_MSG_HDR_SIZE || msg_len > avail ||
            (msg_len % ACCEL_RING_MSG_ALIGN) != 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2R ring %u: invalid msg length %u "
                          "(avail=%u)\n", ring->slot, msg_len, avail);
            break;
        }

        /* Dispatch by message type */
        uint16_t msg_type = le16_to_cpu(msg->type);

        switch (msg_type) {
        case ACCEL_RING_MSG_DATA:
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: P2R DATA msg: slot=%u len=%u\n",
                          ring->slot, msg_len);
            n->stats.p2p_xfers++;
            n->stats.p2p_bytes += msg_len - ACCEL_RING_MSG_HDR_SIZE;
            break;

        case ACCEL_RING_MSG_NOTIFY:
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: P2R NOTIFY msg: slot=%u\n",
                          ring->slot);
            break;

        case ACCEL_RING_MSG_STATUS:
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: P2R STATUS msg: slot=%u len=%u\n",
                          ring->slot, msg_len);
            break;

        default:
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2R unknown msg type 0x%x in slot %u\n",
                          msg_type, ring->slot);
            break;
        }

        /* Advance head past this message */
        ring->head = (ring->head + msg_len) % data_size;
        n->stats.cmd_processed++;
    }

    /* Update head in ring header so peer can see consumer progress */
    hdr->head = cpu_to_le32(ring->head);
}

/**
 * accel_p2p_ring_doorbell - Handle P2P ring doorbell writes
 * @n: Device state
 * @offset: Offset within P2P doorbell region (relative to 0x4000)
 * @val: Doorbell value (new tail offset)
 *
 * Called from accel_mmio_write() when a peer device writes to our
 * P2P ring doorbell registers via cross-device MMIO.
 */
void accel_p2p_ring_doorbell(PCIeAccel *n, hwaddr offset, uint32_t val)
{
    uint32_t slot = offset / ACCEL_P2R_DB_STRIDE;
    uint32_t db_off = offset % ACCEL_P2R_DB_STRIDE;

    if (slot >= ACCEL_P2R_MAX_SLOTS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2R doorbell invalid slot %u\n", slot);
        return;
    }

    AccelP2PRing *ring = &n->p2p.p2p_rings[slot];
    if (!ring->active) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: P2R doorbell for inactive slot %u\n", slot);
        return;
    }

    if (db_off == 0) {
        /* Tail doorbell - peer produced new messages */
        uint32_t new_tail = val;

        if (new_tail >= ring->size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2R tail doorbell value %u >= size %u\n",
                          new_tail, ring->size);
            return;
        }

        ring->tail = new_tail;

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: P2R tail doorbell: slot=%u new_tail=%u "
                      "(head=%u)\n", slot, new_tail, ring->head);

        /* Schedule ring processing */
        qemu_bh_schedule(ring->bh);
    }
    /* db_off == 4 is reserved, ignore */
}

/**
 * accel_p2p_ring_reset - Reset all P2P ring buffers
 * @n: Device state
 *
 * Called during device reset. Cancels all BHs and clears state.
 */
void accel_p2p_ring_reset(PCIeAccel *n)
{
    for (int i = 0; i < ACCEL_P2R_MAX_SLOTS; i++) {
        AccelP2PRing *ring = &n->p2p.p2p_rings[i];

        if (ring->bh) {
            qemu_bh_cancel(ring->bh);
            qemu_bh_delete(ring->bh);
            ring->bh = NULL;
        }

        memset(ring, 0, sizeof(*ring));
    }

    n->p2p.num_p2p_rings = 0;
}

/**
 * accel_p2p_ring_cleanup - Cleanup P2P ring resources on device exit
 * @n: Device state
 */
void accel_p2p_ring_cleanup(PCIeAccel *n)
{
    accel_p2p_ring_reset(n);
}
