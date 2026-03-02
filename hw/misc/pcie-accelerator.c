/*
 * QEMU PCIe Accelerator Device - Main Implementation
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This file implements a PCIe accelerator device with:
 * - Submission/completion queues with doorbell registers
 * - PCIe peer-to-peer (P2P) DMA with N:N concurrent transfers
 * - PASID/SVA support for shared virtual addressing
 * - MSI-X interrupts with coalescing
 * - Production-level error handling and logging
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_aer.h"
#include "hw/pci/msix.h"
#include "hw/pci/msi.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "system/hostmem.h"
#include "qom/object.h"

#include "hw/misc/pcie-accelerator.h"
#include "hw/misc/pcie-accelerator-regs.h"

/* Status code for deferred completion */
#define ACCEL_NO_COMPLETE 0xFFFF

/*
 * ===== DMA Helper Functions =====
 *
 * Safe DMA operations with comprehensive error checking and logging.
 */

/**
 * accel_dma_read_safe - Safely read from host memory via DMA
 * @n: Device state
 * @addr: Host physical address
 * @buf: Destination buffer
 * @len: Transfer length
 *
 * Returns: ACCEL_SC_SUCCESS on success, error code on failure
 */
uint16_t accel_dma_read_safe(PCIeAccel *n, uint64_t addr, void *buf, size_t len)
{
    PCIDevice *pci = PCI_DEVICE(n);
    MemTxResult result;

    result = pci_dma_read(pci, addr, buf, len);

    switch (result) {
    case MEMTX_OK:
        return ACCEL_SC_SUCCESS;

    case MEMTX_DECODE_ERROR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: DMA decode error at 0x%" PRIx64 " len %zu\n",
                      addr, len);
        n->stats.dma_errors++;
        return ACCEL_SC_DMA_DECODE_ERROR;

    case MEMTX_ERROR:
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: DMA read error at 0x%" PRIx64 " len %zu\n",
                      addr, len);
        n->stats.dma_errors++;
        return ACCEL_SC_DMA_ERROR;
    }
}

/**
 * accel_dma_write_safe - Safely write to host memory via DMA
 * @n: Device state
 * @addr: Host physical address
 * @buf: Source buffer
 * @len: Transfer length
 *
 * Returns: ACCEL_SC_SUCCESS on success, error code on failure
 */
uint16_t accel_dma_write_safe(PCIeAccel *n, uint64_t addr, const void *buf,
                               size_t len)
{
    PCIDevice *pci = PCI_DEVICE(n);
    MemTxResult result;

    result = pci_dma_write(pci, addr, buf, len);

    switch (result) {
    case MEMTX_OK:
        return ACCEL_SC_SUCCESS;

    case MEMTX_DECODE_ERROR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: DMA decode error at 0x%" PRIx64 " len %zu\n",
                      addr, len);
        n->stats.dma_errors++;
        return ACCEL_SC_DMA_DECODE_ERROR;

    case MEMTX_ERROR:
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: DMA write error at 0x%" PRIx64 " len %zu\n",
                      addr, len);
        n->stats.dma_errors++;
        return ACCEL_SC_DMA_ERROR;
    }
}

/*
 * ===== Controller State Management =====
 */

/**
 * accel_set_ctrl_ready - Set controller ready status
 * @n: Device state
 * @ready: True to set ready, false to clear
 *
 * Updates the CSTS.RDY bit to indicate controller readiness.
 */
void accel_set_ctrl_ready(PCIeAccel *n, bool ready)
{
    if (ready) {
        n->bar.csts |= (1 << ACCEL_CSTS_RDY_SHIFT);
    } else {
        n->bar.csts &= ~(1 << ACCEL_CSTS_RDY_SHIFT);
    }
}

/**
 * accel_set_ctrl_fatal - Set controller fatal error status
 * @n: Device state
 *
 * Sets the CSTS.CFS bit and disables the controller. This is an
 * unrecoverable error requiring a controller reset.
 */
void accel_set_ctrl_fatal(PCIeAccel *n)
{
    n->bar.csts |= (1 << ACCEL_CSTS_CFS_SHIFT);
    n->bar.cc &= ~(1 << ACCEL_CC_EN_SHIFT);  /* Disable controller */

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pcie-accel: FATAL ERROR - controller halted\n");
}

/*
 * ===== Interrupt Handling =====
 */

/**
 * accel_irq_assert - Assert interrupt for a completion queue (MSI/MSI-X only)
 * @n: Device state
 * @cq: Completion queue
 *
 * Implements interrupt coalescing based on threshold and time settings.
 */
void accel_irq_assert(PCIeAccel *n, AccelCQueue *cq)
{
    PCIDevice *pci = PCI_DEVICE(n);

    if (!cq->irq_enabled) {
        return;
    }

    /*
     * Interrupt coalescing logic:
     * - Fire immediately if threshold is 0 (no coalescing)
     * - Otherwise, fire when pending count >= threshold
     */
    uint32_t pending = accel_cq_pending(cq);
    bool fire_irq = false;

    if (n->intcoal_thresh == 0) {
        fire_irq = true;
    } else if (pending >= n->intcoal_thresh) {
        fire_irq = true;
    }
    /* TODO: Time-based coalescing would use a timer here */

    if (fire_irq) {
        if (msix_enabled(pci)) {
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: MSI-X notify: cqid=%u vector=%u\n",
                          cq->cqid, cq->vector);
            msix_notify(pci, cq->vector);
        } else if (msi_enabled(pci)) {
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: MSI notify: cqid=%u vector=%u\n",
                          cq->cqid, cq->vector);
            MSIMessage msg = msi_get_message(pci, cq->vector);
            pci_dma_write(pci, msg.address, &msg.data, sizeof(msg.data));
        }
    }
}

/**
 * accel_irq_deassert - Deassert interrupt for a completion queue
 * @n: Device state
 * @cq: Completion queue
 *
 * MSI/MSI-X are edge-triggered, so deassert is a no-op.
 */
void accel_irq_deassert(PCIeAccel *n, AccelCQueue *cq)
{
    if (!cq->irq_enabled) {
        return;
    }

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: CQ %u deasserting IRQ\n", cq->cqid);
}

/*
 * ===== Completion Queue Management =====
 */

/**
 * accel_enqueue_req_completion - Enqueue request for completion
 * @cq: Completion queue
 * @req: Request to complete
 *
 * Moves request from in-flight list to completion list and schedules
 * the completion posting bottom-half.
 */
void accel_enqueue_req_completion(AccelCQueue *cq, AccelRequest *req)
{
    AccelSQueue *sq = req->sq;

    assert(cq->cqid == sq->cqid);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: Enqueue completion: cqid=%u sqid=%u cid=%u status=%u\n",
                  cq->cqid, sq->sqid, le16_to_cpu(req->cqe.cid), req->status);

    /* Remove from in-flight list */
    QTAILQ_REMOVE(&sq->out_req_list, req, entry);

    /* Add to completion queue's pending list */
    QTAILQ_INSERT_TAIL(&cq->req_list, req, entry);

    /* Schedule asynchronous completion posting */
    qemu_bh_schedule(cq->bh);
}

/**
 * accel_post_cqes - Post completion queue entries to host memory
 * @opaque: Completion queue pointer
 *
 * Bottom-half handler that writes CQEs to host memory and fires interrupts.
 * Implements the phase bit protocol for lock-free completion
 * detection.
 */
void accel_post_cqes(void *opaque)
{
    AccelCQueue *cq = opaque;
    PCIeAccel *n = cq->ctrl;
    AccelRequest *req, *next;
    bool pending_before = (cq->head != cq->tail);
    uint16_t status;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: post_cqes called for CQ %u (head=%u tail=%u)\n",
                  cq->cqid, cq->head, cq->tail);

    QTAILQ_FOREACH_SAFE(req, &cq->req_list, entry, next) {
        AccelSQueue *sq = req->sq;
        uint64_t addr;

        /* Stop if CQ is full */
        if (accel_cq_full(cq)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: CQ %u full, deferring completions\n",
                          cq->cqid);
            break;
        }

        /* Build completion entry */
        req->cqe.sq_head = cpu_to_le16(sq->head);

        /* Set status with phase bit (32-bit): SC=req->status, SCT=0, P=cq->phase */
        req->cqe.status = cpu_to_le32(ACCEL_CQE_BUILD_STATUS(req->status,
                                                               0, cq->phase));

        /* Write CQE to host memory (via CXL.cache D2H Write or PCIe DMA) */
        addr = cq->dma_addr + (cq->tail << ACCEL_CQES);
        status = n->dma_ops.write(n, addr, &req->cqe, sizeof(req->cqe));

        if (status != ACCEL_SC_SUCCESS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Failed to write CQE at 0x%" PRIx64 "\n",
                          addr);
            accel_set_ctrl_fatal(n);
            break;
        }

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: CQ %u: SQ %u CID %u status 0x%04x\n",
                      cq->cqid, sq->sqid, req->cqe.cid, req->cqe.status);

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: CQ[%u] POST @ 0x%" PRIx64 " tail=%u phase=%u\n"
                 "  sq_head=%u cid=%u status=0x%08x result=0x%016" PRIx64 "\n",
                 cq->cqid, addr, cq->tail, cq->phase,
                 le16_to_cpu(req->cqe.sq_head),
                 le16_to_cpu(req->cqe.cid),
                 le32_to_cpu(req->cqe.status),
                 le64_to_cpu(req->cqe.result));

        /* Remove from completion list */
        QTAILQ_REMOVE(&cq->req_list, req, entry);

        /* Increment CQ tail (with phase toggle on wrap) */
        accel_inc_cq_tail(cq);

        /* Free DMA resources if allocated */
        if (req->bounce_buf) {
            g_free(req->bounce_buf);
            req->bounce_buf = NULL;
            req->bounce_len = 0;
        }

        /* Return request to free pool */
        QTAILQ_INSERT_TAIL(&sq->req_list, req, entry);

        /* Update statistics */
        n->stats.cmd_completed++;

        /* Schedule SQ processing if requests available */
        if (QTAILQ_EMPTY(&sq->req_list) == false && !accel_sq_empty(sq)) {
            qemu_bh_schedule(sq->bh);
        }
    }

    /* Fire interrupt if new completions posted */
    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: post_cqes IRQ check: tail=%u head=%u pending_before=%d irq_en=%d\n",
                  cq->tail, cq->head, pending_before, cq->irq_enabled);
    if (cq->tail != cq->head) {
        if (!pending_before && cq->irq_enabled) {
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: Firing IRQ for CQ %u (vector=%u)\n",
                          cq->cqid, cq->vector);
            accel_irq_assert(n, cq);
        }
    } else if (pending_before && cq->irq_enabled) {
        /* CQ became empty */
        accel_irq_deassert(n, cq);
    }

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: post_cqes done for CQ %u (head=%u tail=%u irq_en=%d)\n",
                  cq->cqid, cq->head, cq->tail, cq->irq_enabled);
}

/*
 * ===== Command Validation =====
 */

/**
 * accel_validate_cmd - Validate command parameters
 * @n: Device state
 * @cmd: Command to validate
 *
 * Returns: ACCEL_SC_SUCCESS if valid, error code otherwise
 */
static uint16_t accel_validate_cmd(PCIeAccel *n, AccelCmd *cmd)
{
    /* Validate opcode range */
    if (cmd->opcode == 0 || cmd->opcode > ACCEL_CMD_MEM_WRITE) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Invalid opcode 0x%x\n", cmd->opcode);
        return ACCEL_SC_INVALID_OPCODE;
    }

    /* Validate data pointers for commands that need them */
    switch (cmd->opcode) {
    case ACCEL_CMD_LOOPBACK:
    case ACCEL_CMD_P2P_WRITE:
    case ACCEL_CMD_P2P_READ:
        if (cmd->dbd.prpl.prp1 == 0) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: NULL PRP1 for opcode 0x%x\n",
                          cmd->opcode);
            return ACCEL_SC_INVALID_PRP;
        }
        break;

    default:
        break;
    }

    /* Validate P2P-specific parameters */
    if (cmd->opcode == ACCEL_CMD_P2P_WRITE ||
        cmd->opcode == ACCEL_CMD_P2P_READ) {
        uint32_t len = le32_to_cpu(cmd->dw.p2p.length);

        if (len == 0 || len > (16 * MiB)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Invalid P2P length %u\n", len);
            return ACCEL_SC_P2P_LEN_INVALID;
        }

        uint16_t peer_bdf = le32_to_cpu(cmd->dw.p2p.peer_bdf);
        if (!accel_find_p2p_peer(n, peer_bdf)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: P2P peer 0x%x not found\n", peer_bdf);
            return ACCEL_SC_P2P_PEER_NOT_FOUND;
        }
    }

    /* Validate PASID if enabled */
    if (cmd->flags & ACCEL_CMD_FLAGS_PASID_EN) {
        if (!n->sva.enabled) {
            return ACCEL_SC_PASID_NOT_ENABLED;
        }

        uint32_t pasid = le32_to_cpu(cmd->dw.p2p.pasid);
        if (pasid >= (1U << n->sva.pasid_width)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Invalid PASID %u (width=%u)\n",
                          pasid, n->sva.pasid_width);
            return ACCEL_SC_PASID_INVALID;
        }
    }

    return ACCEL_SC_SUCCESS;
}

/*
 * ===== I/O Command Handlers =====
 */

/**
 * accel_cmd_loopback - Loopback test command
 * @n: Device state
 * @req: Request structure
 *
 * Reads data from PRP1, optionally modifies it, and writes it back.
 * Used for basic functionality testing and benchmarking.
 *
 * Returns: Status code
 */
uint16_t accel_cmd_loopback(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint32_t length = le32_to_cpu(cmd->dw.loopback.length);
    uint64_t prp1 = le64_to_cpu(cmd->dbd.prpl.prp1);
    uint16_t status;
    void *buf;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: LOOPBACK cmd: prp1=0x%" PRIx64 " length=%u\n",
                  prp1, length);

    /* Validate length */
    if (length == 0 || length > (1 * MiB)) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: LOOPBACK invalid length %u\n", length);
        return ACCEL_SC_INVALID_FIELD;
    }

    /* Allocate bounce buffer */
    buf = g_malloc(length);
    if (!buf) {
        return ACCEL_SC_INTERNAL_ERROR;
    }

    /* Read data from host memory */
    status = accel_dma_read_safe(n, prp1, buf, length);
    if (status != ACCEL_SC_SUCCESS) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: LOOPBACK DMA read failed: status=%u\n",
                      status);
        g_free(buf);
        return status;
    }
    qemu_log_mask(LOG_UNIMP, "pcie-accel: LOOPBACK DMA read OK\n");

    /* Optional: Modify data based on pattern */
    uint32_t pattern = le32_to_cpu(cmd->dw.loopback.pattern);
    if (pattern != 0) {
        /* XOR data with pattern for verification */
        uint32_t *data = (uint32_t *)buf;
        for (uint32_t i = 0; i < length / 4; i++) {
            data[i] ^= pattern;
        }
    }

    /* Write data back to host memory */
    status = accel_dma_write_safe(n, prp1, buf, length);
    g_free(buf);

    if (status == ACCEL_SC_SUCCESS) {
        req->cqe.result = cpu_to_le64(length);
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: LOOPBACK completed successfully, len=%u\n",
                      length);
    } else {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: LOOPBACK DMA write failed: status=%u\n",
                      status);
    }

    return status;
}

/**
 * accel_resolve_dev_addr - Resolve DPA address to RAM pointer
 * @n: Device state
 * @dev_addr: Device Physical Address (offset into DPA memory)
 * @length: Access length in bytes
 *
 * Maps a DPA address to the backing RAM pointer.  DPA is a separate
 * address space from MMIO/CMB — addresses start at 0.
 *
 * Returns: RAM pointer on success, NULL if address out of range
 */
static void *accel_resolve_dev_addr(PCIeAccel *n, uint64_t dev_addr,
                                    uint32_t length)
{
    if (n->dpa_mr) {
        uint64_t dpa_size = memory_region_size(n->dpa_mr);
        if (dev_addr + length <= dpa_size) {
            return (uint8_t *)memory_region_get_ram_ptr(n->dpa_mr) + dev_addr;
        }
    }

    return NULL;
}


/**
 * accel_prp_transfer - DMA transfer using PRP list
 * @n: Device state
 * @prp1: First PRP entry (may have sub-page offset)
 * @prp2: Second PRP entry or PRP list pointer
 * @buf: Device-side buffer
 * @length: Transfer length in bytes
 * @is_write: true = write to host (device->host), false = read from host
 *
 * NVMe-style PRP semantics:
 * - PRP1 may have a sub-page offset; subsequent PRPs are page-aligned
 * - Transfer fits in one page: only PRP1 used
 * - Transfer spans two pages: PRP2 is the second page address
 * - Transfer spans >2 pages: PRP2 points to a PRP list (page of entries)
 *
 * Returns: ACCEL_SC_SUCCESS or error status code
 */
static uint16_t accel_prp_transfer(PCIeAccel *n, uint64_t prp1, uint64_t prp2,
                                    void *buf, uint32_t length, bool is_write)
{
    uint32_t page_size = n->page_size;
    uint32_t offset = prp1 & (page_size - 1);
    uint32_t first_chunk = MIN(length, page_size - offset);
    uint8_t *p = buf;
    uint16_t status;
    uint32_t remaining;

    /* Transfer first page (may be partial due to sub-page offset) */
    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: PRP[0] addr 0x%" PRIx64
                  " chunk %u write %d\n",
                  prp1, first_chunk, is_write);
    if (is_write) {
        status = accel_dma_write_safe(n, prp1, p, first_chunk);
    } else {
        status = accel_dma_read_safe(n, prp1, p, first_chunk);
    }
    if (status != ACCEL_SC_SUCCESS) {
        return status;
    }

    p += first_chunk;
    remaining = length - first_chunk;

    if (remaining == 0) {
        return ACCEL_SC_SUCCESS;
    }

    if (remaining <= page_size) {
        /* PRP2 is a direct PRP entry (second page) */
        if (prp2 == 0) {
            return ACCEL_SC_INVALID_PRP;
        }
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: PRP[1] addr 0x%" PRIx64
                      " chunk %u write %d\n",
                      prp2, remaining, is_write);
        if (is_write) {
            return accel_dma_write_safe(n, prp2, p, remaining);
        } else {
            return accel_dma_read_safe(n, prp2, p, remaining);
        }
    }

    /* PRP2 points to a PRP list (array of page-aligned addresses) */
    if (prp2 == 0) {
        return ACCEL_SC_INVALID_PRP;
    }

    uint32_t max_prps = page_size / sizeof(uint64_t);
    uint64_t *prp_list = g_malloc(page_size);

    status = accel_dma_read_safe(n, prp2, prp_list, page_size);
    if (status != ACCEL_SC_SUCCESS) {
        g_free(prp_list);
        return status;
    }

    for (uint32_t i = 0; i < max_prps && remaining > 0; i++) {
        uint64_t prp_entry = le64_to_cpu(prp_list[i]);
        uint32_t chunk;

        if (prp_entry == 0) {
            g_free(prp_list);
            return ACCEL_SC_INVALID_PRP;
        }

        chunk = MIN(remaining, page_size);
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: PRP[%u] addr 0x%" PRIx64
                      " chunk %u write %d\n",
                      i + 2, prp_entry, chunk, is_write);
        if (is_write) {
            status = accel_dma_write_safe(n, prp_entry, p, chunk);
        } else {
            status = accel_dma_read_safe(n, prp_entry, p, chunk);
        }
        if (status != ACCEL_SC_SUCCESS) {
            g_free(prp_list);
            return status;
        }

        p += chunk;
        remaining -= chunk;
    }

    g_free(prp_list);

    if (remaining > 0) {
        return ACCEL_SC_INVALID_PRP;
    }

    return ACCEL_SC_SUCCESS;
}

/* SGL descriptor for reading chained segments from host memory (16 bytes) */
typedef struct QEMU_PACKED AccelSglDesc {
    uint64_t addr;
    uint32_t length;
    uint8_t  reserved[3];
    uint8_t  type;          /* SGL descriptor type (ACCEL_SGL_DESC_*) */
} AccelSglDesc;

/**
 * accel_sgl_transfer - DMA transfer using SGL descriptors
 * @n: Device state
 * @sgl_addr: Address from first SGL descriptor
 * @sgl_length: Length from first SGL descriptor
 * @sgl_type: Type byte from first SGL descriptor (ACCEL_SGL_DESC_*)
 * @buf: Device-side buffer
 * @length: Total transfer length in bytes
 * @is_write: true = write to host, false = read from host
 *
 * NVMe-style SGL semantics:
 * - Data Block (0x00): addr + length describe a contiguous host buffer
 * - Segment (0x02): addr points to array of SGL descriptors, length = array size
 * - Last Segment (0x03): like Segment, final array in the chain
 * - Chain pointers must be the last entry in a Segment descriptor array
 *
 * For Data Block: sgl_length is the host buffer size (transfer is
 * MIN(sgl_length, length) bytes).
 * For Segment/Last Segment: sgl_length is the size of the descriptor array.
 *
 * Returns: ACCEL_SC_SUCCESS or error status code
 */
static uint16_t accel_sgl_transfer(PCIeAccel *n, uint64_t sgl_addr,
                                    uint32_t sgl_length, uint8_t sgl_type,
                                    void *buf, uint32_t length, bool is_write)
{
    uint8_t *p = buf;
    uint32_t remaining = length;
    uint16_t status;

    /* Single Data Block — direct contiguous transfer */
    if (sgl_type == ACCEL_SGL_DESC_DATA_BLOCK) {
        uint32_t chunk = MIN(sgl_length, remaining);
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: SGL[0] addr 0x%" PRIx64
                      " length %u type %u write %d\n",
                      sgl_addr, chunk, sgl_type, is_write);
        if (is_write) {
            return accel_dma_write_safe(n, sgl_addr, p, chunk);
        } else {
            return accel_dma_read_safe(n, sgl_addr, p, chunk);
        }
    }

    /* Segment / Last Segment: walk the descriptor chain */
    if (sgl_type != ACCEL_SGL_DESC_SEGMENT &&
        sgl_type != ACCEL_SGL_DESC_LAST_SEGMENT) {
        return ACCEL_SC_INVALID_FIELD;
    }

    uint64_t seg_addr = sgl_addr;
    uint32_t seg_len = sgl_length;
    bool more_segments = true;

    while (more_segments && remaining > 0) {
        uint32_t num_descs;
        AccelSglDesc *descs;

        if (seg_len == 0 || seg_len % sizeof(AccelSglDesc) != 0) {
            return ACCEL_SC_INVALID_FIELD;
        }

        num_descs = seg_len / sizeof(AccelSglDesc);
        descs = g_malloc(seg_len);

        status = accel_dma_read_safe(n, seg_addr, descs, seg_len);
        if (status != ACCEL_SC_SUCCESS) {
            g_free(descs);
            return status;
        }

        more_segments = false;

        for (uint32_t i = 0; i < num_descs && remaining > 0; i++) {
            uint64_t d_addr = le64_to_cpu(descs[i].addr);
            uint32_t d_len = le32_to_cpu(descs[i].length);
            uint8_t d_type = descs[i].type;

            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: SGL[%u] addr 0x%" PRIx64
                          " length %u type %u write %d\n",
                          i, d_addr, d_len, d_type, is_write);

            if (d_type == ACCEL_SGL_DESC_DATA_BLOCK) {
                uint32_t chunk = MIN(d_len, remaining);
                if (is_write) {
                    status = accel_dma_write_safe(n, d_addr, p, chunk);
                } else {
                    status = accel_dma_read_safe(n, d_addr, p, chunk);
                }
                if (status != ACCEL_SC_SUCCESS) {
                    g_free(descs);
                    return status;
                }
                p += chunk;
                remaining -= chunk;

            } else if ((d_type == ACCEL_SGL_DESC_SEGMENT ||
                        d_type == ACCEL_SGL_DESC_LAST_SEGMENT) &&
                       i == num_descs - 1) {
                /* Chain pointer: must be last entry in segment */
                seg_addr = d_addr;
                seg_len = d_len;
                more_segments = true;

            } else {
                g_free(descs);
                return ACCEL_SC_INVALID_FIELD;
            }
        }

        g_free(descs);
    }

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_host_dma_transfer - Scatter-gather DMA transfer to/from host
 * @n: Device state
 * @cmd: Command (contains DBD with host buffer descriptors)
 * @buf: Device-side contiguous buffer
 * @length: Transfer length in bytes
 * @is_write: true = device writes to host, false = device reads from host
 *
 * Decodes the Data Block Descriptor and performs the DMA transfer.
 * Supports:
 *   PRPL: PRP list (single entry, two entries, or PRP list via prp2)
 *   SGL:  Scatter-gather (Data Block, Segment chain, Last Segment)
 *   SVA:  Single virtual address (direct contiguous transfer)
 *
 * Returns: ACCEL_SC_SUCCESS or error status code
 */
static uint16_t accel_host_dma_transfer(PCIeAccel *n, AccelCmd *cmd,
                                         void *buf, uint32_t length,
                                         bool is_write)
{
    uint8_t dbd_type = cmd->flags & ACCEL_CMD_FLAGS_DBD_MASK;

    switch (dbd_type) {
    case ACCEL_CMD_FLAGS_DBD_PRPL: {
        uint64_t prp1 = le64_to_cpu(cmd->dbd.prpl.prp1);
        uint64_t prp2 = le64_to_cpu(cmd->dbd.prpl.prp2);
        return accel_prp_transfer(n, prp1, prp2, buf, length, is_write);
    }

    case ACCEL_CMD_FLAGS_DBD_SGL: {
        uint64_t addr = le64_to_cpu(cmd->dbd.sgl.addr);
        uint32_t sgl_len = le32_to_cpu(cmd->dbd.sgl.length);
        uint8_t sgl_type = cmd->dbd.sgl.type;
        /*
         * For Data Block descriptors, sgl_len is the host buffer size.
         * If it's 0 (e.g. MEM_READ/MEM_WRITE overlay zeros the field),
         * fall back to the caller-provided transfer length.
         */
        if (sgl_type == ACCEL_SGL_DESC_DATA_BLOCK && sgl_len == 0) {
            sgl_len = length;
        }
        return accel_sgl_transfer(n, addr, sgl_len, sgl_type,
                                   buf, length, is_write);
    }

    case ACCEL_CMD_FLAGS_DBD_SVA: {
        uint64_t addr = le64_to_cpu(cmd->dbd.sva.addr);
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: SVA addr 0x%" PRIx64
                      " length %u write %d\n",
                      addr, length, is_write);
        if (is_write) {
            return accel_dma_write_safe(n, addr, buf, length);
        } else {
            return accel_dma_read_safe(n, addr, buf, length);
        }
    }

    default:
        return ACCEL_SC_INVALID_FIELD;
    }
}

/**
 * accel_log_data_dump - Dump buffer contents, compressing repeated chunks
 * @tag: Label string (e.g. "MEM_READ", "MEM_WRITE")
 * @buf: Data buffer
 * @length: Buffer length in bytes
 * @cid: Command ID
 *
 * Logs one line per 16-byte chunk via qemu_log_mask.  Runs of identical
 * chunks are compressed: the first is printed normally, then a single
 * "* (repeats N times, ...)" line replaces the duplicates.
 */
static void accel_log_data_dump(const char *tag, const void *buf,
                                uint32_t length, uint16_t cid)
{
    const uint64_t *d = (const uint64_t *)buf;
    uint32_t nchunks = length / 16;
    uint32_t tail = length & 0xF;
    uint32_t i;
    uint32_t run_start = 0;     /* first chunk index of current run */
    uint64_t run_d0 = 0, run_d1 = 0;

    for (i = 0; i < nchunks; i++) {
        uint64_t d0 = le64_to_cpu(d[i * 2]);
        uint64_t d1 = le64_to_cpu(d[i * 2 + 1]);

        if (i > 0 && d0 == run_d0 && d1 == run_d1) {
            continue;   /* still in a run of identical chunks */
        }

        /* Flush previous run if it had duplicates */
        if (i > 0 && i - run_start > 1) {
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: %s data: cid %u  "
                          "* (repeats %u times, +0x%04x..+0x%04x)\n",
                          tag, cid, i - run_start,
                          run_start * 16, (i - 1) * 16);
        }

        /* Print this new distinct chunk */
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: %s data: cid %u +0x%04x"
                      " [0x%016" PRIx64 " 0x%016" PRIx64 "]\n",
                      tag, cid, i * 16, d0, d1);

        run_start = i;
        run_d0 = d0;
        run_d1 = d1;
    }

    /* Flush final run if it had duplicates */
    if (nchunks > 0 && nchunks - run_start > 1) {
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: %s data: cid %u  "
                      "* (repeats %u times, +0x%04x..+0x%04x)\n",
                      tag, cid, nchunks - run_start,
                      run_start * 16, (nchunks - 1) * 16);
    }

    /* Handle trailing bytes (< 16) */
    if (tail >= 8) {
        uint64_t d0 = le64_to_cpu(d[nchunks * 2]);
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: %s data: cid %u +0x%04x"
                      " [0x%016" PRIx64 "]\n",
                      tag, cid, nchunks * 16, d0);
    }
}

/**
 * accel_cmd_mem_read - Read from device memory to host
 * @n: Device state
 * @req: Request structure
 *
 * Reads data from DPA memory and DMA writes to host.
 * Host buffer is decoded from the DBD union (NVMe-style):
 *   PRPL: addr=prp1, length from command
 *   SGL:  addr and length from SGL descriptor
 *   SVA:  addr=sva.addr, pasid=sva.pasid, length from command
 *
 * Returns: Status code
 */
uint16_t accel_cmd_mem_read(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint64_t dev_addr = le64_to_cpu(cmd->mem_read.dev_addr);
    uint64_t host_addr = le64_to_cpu(cmd->mem_read.host_addr);
    uint32_t length = le32_to_cpu(cmd->mem_read.length);
    uint16_t status;
    void *dev_ptr;
    void *buf;

    /*
     * MEM_READ has an explicit length field (CDW7) — do NOT call
     * accel_resolve_xfer_length() which would incorrectly override
     * length with dbd.sgl.length for SGL Data Block mode.
     * The SGL descriptor describes the host buffer, not the transfer size.
     */

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: MEM_READ cmd: cid %u dev_addr 0x%" PRIx64
                  " host_addr 0x%" PRIx64 " length %u dbd_type %u\n",
                  le16_to_cpu(cmd->cid), dev_addr, host_addr,
                  length, cmd->flags & ACCEL_CMD_FLAGS_DBD_MASK);

    if (length == 0 || length > (1 * MiB)) {
        return ACCEL_SC_INVALID_FIELD;
    }

    dev_ptr = accel_resolve_dev_addr(n, dev_addr, length);
    if (!dev_ptr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: MEM_READ dev_addr 0x%" PRIx64
                      " + len %u out of range\n", dev_addr, length);
        return ACCEL_SC_LBA_OUT_OF_RANGE;
    }

    buf = g_malloc(length);
    memcpy(buf, dev_ptr, length);

    accel_log_data_dump("MEM_READ", buf, length, le16_to_cpu(cmd->cid));

    status = accel_host_dma_transfer(n, cmd, buf, length, true);
    g_free(buf);

    if (status == ACCEL_SC_SUCCESS) {
        req->cqe.result = cpu_to_le64(length);
    }

    return status;
}

/**
 * accel_cmd_mem_write - Write from host to device memory
 * @n: Device state
 * @req: Request structure
 *
 * DMA reads from host memory and writes to DPA memory.
 * Host buffer is decoded from the DBD union (NVMe-style):
 *   PRPL: addr=prp1, length from command
 *   SGL:  addr and length from SGL descriptor
 *   SVA:  addr=sva.addr, pasid=sva.pasid, length from command
 *
 * Returns: Status code
 */
uint16_t accel_cmd_mem_write(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint64_t dev_addr = le64_to_cpu(cmd->mem_write.dev_addr);
    uint64_t host_addr = le64_to_cpu(cmd->mem_write.host_addr);
    uint32_t length = le32_to_cpu(cmd->mem_write.length);
    uint16_t status;
    void *dev_ptr;
    void *buf;

    /*
     * MEM_WRITE has an explicit length field (CDW7) — do NOT call
     * accel_resolve_xfer_length() which would incorrectly override
     * length with dbd.sgl.length for SGL Data Block mode.
     * The SGL descriptor describes the host buffer, not the transfer size.
     */

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: MEM_WRITE cmd: cid %u dev_addr 0x%" PRIx64
                  " host_addr 0x%" PRIx64 " length %u dbd_type %u\n",
                  le16_to_cpu(cmd->cid), dev_addr, host_addr,
                  length, cmd->flags & ACCEL_CMD_FLAGS_DBD_MASK);

    if (length == 0 || length > (1 * MiB)) {
        return ACCEL_SC_INVALID_FIELD;
    }

    dev_ptr = accel_resolve_dev_addr(n, dev_addr, length);
    if (!dev_ptr) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: MEM_WRITE dev_addr 0x%" PRIx64
                      " + len %u out of range\n", dev_addr, length);
        return ACCEL_SC_LBA_OUT_OF_RANGE;
    }

    buf = g_malloc(length);

    status = accel_host_dma_transfer(n, cmd, buf, length, false);
    if (status != ACCEL_SC_SUCCESS) {
        g_free(buf);
        return status;
    }

    accel_log_data_dump("MEM_WRITE", buf, length, le16_to_cpu(cmd->cid));

    memcpy(dev_ptr, buf, length);
    g_free(buf);

    req->cqe.result = cpu_to_le64(length);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_io_cmd - Dispatch I/O commands
 * @n: Device state
 * @req: Request structure
 *
 * Returns: Status code or ACCEL_NO_COMPLETE for async commands
 */
uint16_t accel_io_cmd(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;

    static const char *io_names[] = {
        [0]                    = "INVALID",
        [ACCEL_CMD_LOOPBACK]   = "LOOPBACK",
        [ACCEL_CMD_P2P_WRITE]  = "P2P_WRITE",
        [ACCEL_CMD_P2P_READ]   = "P2P_READ",
        [4]                    = NULL,
        [ACCEL_CMD_MEM_READ]   = "MEM_READ",
        [ACCEL_CMD_MEM_WRITE]  = "MEM_WRITE",
    };
    const char *name = (cmd->opcode <= ACCEL_CMD_MEM_WRITE && io_names[cmd->opcode])
                       ? io_names[cmd->opcode] : "UNKNOWN";
    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: IO CMD %s (0x%02x) sqid=%u cid=%u\n",
             name, cmd->opcode, req->sq->sqid, le16_to_cpu(cmd->cid));

    switch (cmd->opcode) {
    case ACCEL_CMD_LOOPBACK:
        return accel_cmd_loopback(n, req);

    case ACCEL_CMD_P2P_WRITE:
        return accel_cmd_p2p_write(n, req);

    case ACCEL_CMD_P2P_READ:
        return accel_cmd_p2p_read(n, req);

    case ACCEL_CMD_MEM_READ:
        return accel_cmd_mem_read(n, req);

    case ACCEL_CMD_MEM_WRITE:
        return accel_cmd_mem_write(n, req);

    default:
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: Invalid command: SQ %u opcode 0x%02x\n",
                      req->sq->sqid, cmd->opcode);
        return ACCEL_SC_INVALID_OPCODE;
    }
}

/*
 * ===== Admin Command Handlers =====
 */

/**
 * accel_cmd_identify - Identify command
 * @n: Device state
 * @req: Request structure
 *
 * Returns device capabilities and configuration to the host.
 */
uint16_t accel_cmd_identify(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint64_t prp1 = le64_to_cpu(cmd->dbd.prpl.prp1);
    AccelIdData id = {0};
    uint32_t nr = 0;

    /* Hardware info */
    id.hw_info.tid = 0;
    id.hw_info.dev_id = cpu_to_le16(n->dev_id);
    id.hw_info.ccnt = cpu_to_le16(16);
    id.hw_info.pcnt = cpu_to_le16(8);
    id.hw_info.cmem = cpu_to_le64(48ULL * 1024 * 1024 * 1024);
    id.hw_info.csmem = cpu_to_le32(10 * 1024 * 1024);

    /* Memory regions - report device-internal DPA region */
    if (n->dpa_mr) {
        uint64_t dpa_size = memory_region_size(n->dpa_mr);
        id.mem_regions[nr].desc = cpu_to_le64(
            ACCEL_MR_DESC(0, ACCEL_MR_TYPE_MEM, 0, dpa_size));
        id.mem_regions[nr].addr = cpu_to_le64(0);
        nr++;
    }

    id.mem_region_count = cpu_to_le32(nr);
    id.data_len = cpu_to_le32(sizeof(id));

    return accel_dma_write_safe(n, prp1, &id, sizeof(id));
}

/**
 * accel_cmd_create_ioq - Create I/O queue pair (CQ + SQ)
 * @n: Device state
 * @req: Request structure
 *
 * Creates both a completion queue and submission queue with the same qid.
 * Queue depth is derived from the CAP register DEPTH field.
 */
uint16_t accel_cmd_create_ioq(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t qid = le16_to_cpu(cmd->create_ioq.qid);
    uint16_t vector = le16_to_cpu(cmd->create_ioq.irq_vector);
    uint64_t sq_base = le64_to_cpu(cmd->create_ioq.sq_base);
    uint64_t cq_base = le64_to_cpu(cmd->create_ioq.cq_base);
    uint32_t depth_val = (n->bar.cap >> ACCEL_CAP_DEPTH_SHIFT) & ACCEL_CAP_DEPTH_MASK;
    uint32_t qsize = 1 << depth_val;
    uint16_t irq_en = (vector > 0) ? 1 : 0;
    AccelCQueue *cq;
    AccelSQueue *sq;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: CREATE_IOQ: qid=%u sq_base=0x%" PRIx64
                  " cq_base=0x%" PRIx64 " vector=%u depth=%u\n",
                  qid, sq_base, cq_base, vector, qsize);

    /* Validate queue ID */
    if (qid == 0 || qid > n->max_ioqpairs) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: CREATE_IOQ invalid qid %u (max=%u)\n",
                      qid, n->max_ioqpairs);
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    if (n->cq[qid] != NULL || n->sq[qid] != NULL) {
        return ACCEL_SC_QUEUE_ALREADY_EXISTS;
    }

    /* Validate address alignment */
    if (cq_base & (n->page_size - 1)) {
        return ACCEL_SC_INVALID_QUEUE_ADDR;
    }
    if (sq_base & (n->page_size - 1)) {
        return ACCEL_SC_INVALID_QUEUE_ADDR;
    }

    /* Validate MSI-X vector */
    if (irq_en && vector >= (n->max_ioqpairs + 1)) {
        return ACCEL_SC_INVALID_IRQ_VECTOR;
    }

    /* Create CQ */
    cq = g_malloc0(sizeof(AccelCQueue));
    accel_init_cq(cq, n, cq_base, qid, vector, qsize, irq_en);
    n->cq[qid] = cq;

    /* Create SQ */
    sq = g_malloc0(sizeof(AccelSQueue));
    accel_init_sq(sq, n, sq_base, qid, qid, qsize);
    n->sq[qid] = sq;

    n->conf_ioqpairs = MAX(n->conf_ioqpairs, qid);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: CREATE_IOQ success: qid=%u size=%u vector=%u\n",
                  qid, qsize, vector);

    /* Update device status */
    n->bar.devstat = (n->bar.devstat & ~(0xFF << ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT)) |
                     ((n->conf_ioqpairs & 0xFF) << ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_cmd_delete_ioq - Delete I/O queue pair (SQ + CQ)
 * @n: Device state
 * @req: Request structure
 *
 * Deletes both the submission queue and completion queue with the given qid.
 */
uint16_t accel_cmd_delete_ioq(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t qid = le16_to_cpu(cmd->delete_ioq.qid);
    AccelSQueue *sq;
    AccelCQueue *cq;

    if (qid == 0 || qid > n->max_ioqpairs) {
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    if (!accel_check_sqid(n, qid) || !accel_check_cqid(n, qid)) {
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    sq = n->sq[qid];
    cq = n->cq[qid];

    /* Abort any in-flight requests on the SQ */
    AccelRequest *req_iter, *next;
    QTAILQ_FOREACH_SAFE(req_iter, &sq->out_req_list, entry, next) {
        req_iter->status = ACCEL_SC_CMD_ABORT_SQID;
        accel_enqueue_req_completion(cq, req_iter);
    }

    /* Delete SQ first, then CQ */
    accel_free_sq(sq, n);
    n->sq[qid] = NULL;

    accel_free_cq(cq, n);
    n->cq[qid] = NULL;

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_admin_cmd - Dispatch admin commands
 * @n: Device state
 * @req: Request structure
 *
 * Returns: Status code or ACCEL_NO_COMPLETE for async commands
 */
uint16_t accel_admin_cmd(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;

    static const char *adm_names[] = {
        [ACCEL_ADM_CMD_IDENTIFY]          = "IDENTIFY",
        [ACCEL_ADM_CMD_SET_FEATURES]      = "SET_FEATURES",
        [ACCEL_ADM_CMD_GET_FEATURES]      = "GET_FEATURES",
        [ACCEL_ADM_CMD_CREATE_IOQ]        = "CREATE_IOQ",
        [ACCEL_ADM_CMD_DELETE_IOQ]        = "DELETE_IOQ",
        [ACCEL_ADM_CMD_P2P_SETUP]         = "P2P_SETUP",
        [ACCEL_ADM_CMD_P2P_TEARDOWN]      = "P2P_TEARDOWN",
    };
    const char *name = (cmd->opcode <= ACCEL_ADM_CMD_P2P_TEARDOWN &&
                         adm_names[cmd->opcode]) ? adm_names[cmd->opcode]
                                                  : "UNKNOWN";
    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: ADMIN CMD %s (0x%02x) cid=%u\n",
             name, cmd->opcode, le16_to_cpu(cmd->cid));

    switch (cmd->opcode) {
    case ACCEL_ADM_CMD_CREATE_IOQ:
        return accel_cmd_create_ioq(n, req);

    case ACCEL_ADM_CMD_DELETE_IOQ:
        return accel_cmd_delete_ioq(n, req);

    case ACCEL_ADM_CMD_IDENTIFY:
        return accel_cmd_identify(n, req);

    case ACCEL_ADM_CMD_P2P_SETUP:
        return accel_cmd_p2p_setup(n, req);

    case ACCEL_ADM_CMD_P2P_TEARDOWN:
        return accel_cmd_p2p_teardown(n, req);

    default:
        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: Invalid command: SQ 0 opcode 0x%02x\n",
                      cmd->opcode);
        return ACCEL_SC_INVALID_OPCODE;
    }
}

/*
 * ===== Submission Queue Processing =====
 */

/**
 * accel_process_sq - Process submission queue entries
 * @opaque: Submission queue pointer
 *
 * Bottom-half handler triggered by doorbell writes. Fetches commands
 * from host memory, validates them, and dispatches for execution.
 */
void accel_process_sq(void *opaque)
{
    AccelSQueue *sq = opaque;
    PCIeAccel *n = sq->ctrl;
    AccelCQueue *cq = n->cq[sq->cqid];
    AccelCmd cmd;
    AccelRequest *req;
    uint64_t addr;
    uint16_t status;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: Processing SQ %u: head %u tail %u\n",
                  sq->sqid, sq->head, sq->tail);

    /* Process commands until queue is empty or no requests available */
    while (!accel_sq_empty(sq) && !QTAILQ_EMPTY(&sq->req_list)) {
        /* Fetch command from host memory (via CXL.cache D2H Read or PCIe DMA) */
        addr = sq->dma_addr + (sq->head << ACCEL_SQES);
        status = n->dma_ops.read(n, addr, &cmd, sizeof(cmd));

        if (status != ACCEL_SC_SUCCESS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Failed to read command from SQ %u at 0x%"
                          PRIx64 "\n", sq->sqid, addr);
            accel_set_ctrl_fatal(n);
            break;
        }

        /* Trace: dump fetched SQE */
        qemu_log_mask(LOG_UNIMP,
                 "pcie-accel: SQ[%u] FETCH @ 0x%" PRIx64 " head=%u\n"
                 "  opcode=0x%02x flags=0x%02x cid=%u\n"
                 "  dbd: prp1=0x%016" PRIx64
                 " prp2=0x%016" PRIx64
                 " sgl.len=%u sgl.type=0x%02x\n"
                 "  data_xfer_size=%u\n"
                 "  cdw10=0x%08x cdw11=0x%08x cdw12=0x%08x\n"
                 "  cdw13=0x%08x cdw14=0x%08x cdw15=0x%08x\n",
                 sq->sqid, addr, sq->head,
                 cmd.opcode, cmd.flags,
                 le16_to_cpu(cmd.cid),
                 le64_to_cpu(cmd.dbd.prpl.prp1),
                 le64_to_cpu(cmd.dbd.prpl.prp2),
                 le32_to_cpu(cmd.dbd.sgl.length),
                 cmd.dbd.sgl.type,
                 le32_to_cpu(cmd.data_xfer_size),
                 le32_to_cpu(cmd.dw.admin.cdw10),
                 le32_to_cpu(cmd.dw.admin.cdw11),
                 le32_to_cpu(cmd.dw.admin.cdw12),
                 le32_to_cpu(cmd.dw.admin.cdw13),
                 le32_to_cpu(cmd.dw.admin.cdw14),
                 le32_to_cpu(cmd.dw.admin.cdw15));

        /* Increment SQ head */
        accel_inc_sq_head(sq);

        /* Get request from free pool */
        req = QTAILQ_FIRST(&sq->req_list);
        QTAILQ_REMOVE(&sq->req_list, req, entry);
        QTAILQ_INSERT_TAIL(&sq->out_req_list, req, entry);

        /* Initialize request */
        memset(&req->cqe, 0, sizeof(req->cqe));
        memcpy(&req->cmd, &cmd, sizeof(cmd));
        req->cqe.cid = cmd.cid;
        req->status = ACCEL_SC_SUCCESS;
        req->bounce_buf = NULL;
        req->bounce_len = 0;

        /* Dispatch command */
        if (sq->sqid == 0) {
            /* Admin queue - admin commands have their own validation */
            status = accel_admin_cmd(n, req);
        } else {
            /* I/O queue - validate before dispatch */
            status = accel_validate_cmd(n, &cmd);
            if (status != ACCEL_SC_SUCCESS) {
                req->status = status;
                accel_enqueue_req_completion(cq, req);
                n->stats.cmd_errors++;
                continue;
            }
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: I/O cmd dispatch: sqid=%u opcode=0x%x cid=%u\n",
                          sq->sqid, cmd.opcode, le16_to_cpu(cmd.cid));
            status = accel_io_cmd(n, req);
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: I/O cmd complete: sqid=%u cid=%u status=%u\n",
                          sq->sqid, le16_to_cpu(cmd.cid), status);
        }

        /* Handle synchronous completion */
        if (status != ACCEL_NO_COMPLETE) {
            req->status = status;
            accel_enqueue_req_completion(cq, req);
        }

        /* Update statistics */
        n->stats.cmd_processed++;
    }
}

/*
 * ===== Doorbell Register Handling =====
 */

/**
 * accel_process_doorbell - Process doorbell register write
 * @n: Device state
 * @addr: Doorbell register address (offset from doorbell base)
 * @val: New doorbell value
 *
 * Handles both SQ and CQ doorbell writes. SQ doorbells trigger command
 * processing, CQ doorbells acknowledge completions.
 */
static void accel_process_doorbell(PCIeAccel *n, hwaddr addr, uint32_t val)
{
    uint32_t qid;
    bool is_cq;

    /* Validate alignment */
    if (addr & 0x3) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Doorbell write not 32-bit aligned: 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }

    /* Calculate queue ID and type (SQ or CQ) */
    uint32_t db_idx = addr / ACCEL_DB_STRIDE;
    is_cq = (db_idx & 1);
    qid = db_idx / 2;

    if (is_cq) {
        /* ===== Completion Queue Doorbell ===== */
        AccelCQueue *cq;
        uint16_t new_head = val & 0xFFFF;

        if (!accel_check_cqid(n, qid)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Invalid CQ ID %u in doorbell write\n",
                          qid);
            return;
        }

        cq = n->cq[qid];

        if (new_head >= cq->size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: CQ %u doorbell value %u >= size %u\n",
                          qid, new_head, cq->size);
            return;
        }

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: CQ doorbell: cqid %u new_head %u\n",
                      qid, new_head);

        cq->head = new_head;

        /* Check if CQ became empty */
        if (cq->tail == cq->head && cq->irq_enabled) {
            accel_irq_deassert(n, cq);
        } else if (cq->tail != cq->head && cq->irq_enabled) {
            /*
             * There are still pending CQEs that the driver hasn't consumed.
             * Fire an interrupt to ensure the driver processes them.
             * This handles the case where multiple completions arrived
             * before the driver could consume the first one - subsequent
             * completions don't fire interrupts (pending_before=true in
             * accel_post_cqes), so we need to re-assert here.
             */
            accel_irq_assert(n, cq);
        }

        /* If CQ was full, schedule completion posting */
        if (!QTAILQ_EMPTY(&cq->req_list)) {
            qemu_bh_schedule(cq->bh);
        }

    } else {
        /* ===== Submission Queue Doorbell ===== */
        AccelSQueue *sq;
        uint16_t new_tail = val & 0xFFFF;

        if (!accel_check_sqid(n, qid)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Invalid SQ ID %u in doorbell write\n",
                          qid);
            return;
        }

        sq = n->sq[qid];

        if (new_tail >= sq->size) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: SQ %u doorbell value %u >= size %u\n",
                          qid, new_tail, sq->size);
            return;
        }

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: SQ doorbell: sqid %u new_tail %u\n",
                      qid, new_tail);

        sq->tail = new_tail;

        /* Schedule command processing */
        qemu_bh_schedule(sq->bh);
    }
}

/*
 * ===== MMIO Register Handlers =====
 */

/**
 * accel_mmio_read - Handle MMIO register reads
 * @opaque: Device state
 * @addr: Register offset
 * @size: Access size
 *
 * Returns: Register value
 */
static uint64_t accel_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIeAccel *n = PCIE_ACCEL(opaque);
    uint64_t val = 0;

    if (addr >= ACCEL_REG_DOORBELL) {
        /* Doorbell registers are write-only */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Read from write-only doorbell register: 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }

    switch (addr) {
    case ACCEL_REG_CAP:
        val = n->bar.cap;
        break;

    case ACCEL_REG_CC:
        val = n->bar.cc;
        break;

    case ACCEL_REG_CSTS:
        val = n->bar.csts;
        break;

    case ACCEL_REG_ASQ:
        val = n->bar.asq;
        break;

    case ACCEL_REG_ACQ:
        val = n->bar.acq;
        break;

    case ACCEL_REG_CMBBAR:
        val = n->bar.cmbbar;
        break;

    case ACCEL_REG_CMBSZ:
        val = n->bar.cmbsz;
        break;

    case ACCEL_REG_P2PCFG:
        val = n->bar.p2pcfg;
        break;

    case ACCEL_REG_P2RCFG:
        val = n->p2p.p2rcfg;
        break;

    case ACCEL_REG_INTCOAL:
        val = n->bar.intcoal;
        break;

    case ACCEL_REG_DEVSTAT:
        /* Update dynamic status fields */
        n->bar.devstat = ((n->p2p.num_peers & 0xFF) << ACCEL_DEVSTAT_NUM_PEERS_SHIFT) |
                         ((0 & 0xFF) << ACCEL_DEVSTAT_ACTIVE_XFERS_SHIFT) |  /* TODO: sum active xfers */
                         ((n->conf_ioqpairs & 0xFF) << ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT);
        val = n->bar.devstat;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Read from unknown register: 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: MMIO read: addr 0x%" PRIx64
                  " val 0x%" PRIx64 " size %u\n",
                  addr, val, size);

    return val;
}

/**
 * accel_mmio_write - Handle MMIO register writes
 * @opaque: Device state
 * @addr: Register offset
 * @data: Value to write
 * @size: Access size
 */
static void accel_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                              unsigned size)
{
    PCIeAccel *n = PCIE_ACCEL(opaque);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: MMIO write: addr 0x%" PRIx64
                  " val 0x%" PRIx64 " size %u\n",
                  addr, data, size);

    /* Handle P2P ring doorbell writes (0x4000+) */
    if (addr >= ACCEL_P2R_DB_BASE &&
        addr < ACCEL_P2R_DB_BASE + ACCEL_P2R_MAX_SLOTS * ACCEL_P2R_DB_STRIDE) {
        accel_p2p_ring_doorbell(n, addr - ACCEL_P2R_DB_BASE, data);
        return;
    }

    /* Handle host queue doorbell writes (0x1000+) */
    if (addr >= ACCEL_REG_DOORBELL) {
        accel_process_doorbell(n, addr - ACCEL_REG_DOORBELL, data);
        return;
    }

    switch (addr) {
    case ACCEL_REG_CC:
        n->bar.cc = data;
        /* Handle enable/disable */
        if (data & (1 << ACCEL_CC_EN_SHIFT)) {
            /* Enable controller */
            if (!(n->bar.csts & (1 << ACCEL_CSTS_RDY_SHIFT))) {
                /* Initialize admin queues if configured */
                if (n->bar.asq && n->bar.acq) {
                    accel_set_ctrl_ready(n, true);
                }
            }
        } else {
            /* Disable controller */
            accel_set_ctrl_ready(n, false);
        }
        break;

    case ACCEL_REG_ASQ:
        if (n->bar.csts & (1 << ACCEL_CSTS_RDY_SHIFT)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Cannot modify ASQ while controller ready\n");
        } else {
            n->bar.asq = data;
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: ASQ=0x%" PRIx64 " size=%u\n",
                          data, ACCEL_ADMIN_QUEUE_SIZE);
            accel_init_sq(&n->admin_sq, n, data, 0, 0,
                          ACCEL_ADMIN_QUEUE_SIZE);
            /* Link admin SQ to sq[0] for doorbell dispatch */
            n->sq[0] = &n->admin_sq;
        }
        break;

    case ACCEL_REG_ACQ:
        if (n->bar.csts & (1 << ACCEL_CSTS_RDY_SHIFT)) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Cannot modify ACQ while controller ready\n");
        } else {
            n->bar.acq = data;
            qemu_log_mask(LOG_UNIMP,
                          "pcie-accel: ACQ=0x%" PRIx64 " size=%u\n",
                          data, ACCEL_ADMIN_QUEUE_SIZE);
            accel_init_cq(&n->admin_cq, n, data, 0, 0,
                          ACCEL_ADMIN_QUEUE_SIZE, 1);
            /* Link admin CQ to cq[0] for doorbell dispatch */
            n->cq[0] = &n->admin_cq;
        }
        break;

    case ACCEL_REG_INTCOAL:
        n->bar.intcoal = data;
        n->intcoal_thresh = (data >> ACCEL_INTCOAL_THRESH_SHIFT) & 0xFF;
        n->intcoal_time = (data >> ACCEL_INTCOAL_TIME_SHIFT) & 0xFF;
        break;

    case ACCEL_REG_CAP:
    case ACCEL_REG_CSTS:
    case ACCEL_REG_CMBBAR:
    case ACCEL_REG_CMBSZ:
    case ACCEL_REG_P2PCFG:
    case ACCEL_REG_P2RCFG:
    case ACCEL_REG_DEVSTAT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Write to read-only register: 0x%" HWADDR_PRIx "\n",
                      addr);
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Write to unknown register: 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }
}

static const MemoryRegionOps accel_mmio_ops = {
    .read = accel_mmio_read,
    .write = accel_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/*
 * ===== Queue Initialization and Cleanup =====
 */

/**
 * accel_init_sq - Initialize submission queue
 * @sq: Submission queue structure
 * @n: Device state
 * @dma_addr: Queue base address in host memory
 * @sqid: Submission queue ID
 * @cqid: Associated completion queue ID
 * @size: Queue size in entries
 */
void accel_init_sq(AccelSQueue *sq, PCIeAccel *n, uint64_t dma_addr,
                   uint16_t sqid, uint16_t cqid, uint16_t size)
{
    sq->ctrl = n;
    sq->sqid = sqid;
    sq->cqid = cqid;
    sq->size = size;
    sq->dma_addr = dma_addr;
    sq->head = 0;
    sq->tail = 0;

    /* Pre-allocate request pool */
    sq->io_req = g_new0(AccelRequest, size);

    /* Initialize request lists */
    QTAILQ_INIT(&sq->req_list);
    QTAILQ_INIT(&sq->out_req_list);

    for (int i = 0; i < size; i++) {
        sq->io_req[i].sq = sq;
        QTAILQ_INSERT_TAIL(&sq->req_list, &sq->io_req[i], entry);
    }

    /* Create bottom-half for async processing */
    sq->bh = qemu_bh_new(accel_process_sq, sq);

    /* Link to completion queue */
    if (n->cq[cqid]) {
        QTAILQ_INSERT_TAIL(&n->cq[cqid]->sq_list, sq, entry);
    }
}

/**
 * accel_init_cq - Initialize completion queue
 * @cq: Completion queue structure
 * @n: Device state
 * @dma_addr: Queue base address in host memory
 * @cqid: Completion queue ID
 * @vector: MSI-X vector number
 * @size: Queue size in entries
 * @irq_enabled: Interrupt enabled flag
 */
void accel_init_cq(AccelCQueue *cq, PCIeAccel *n, uint64_t dma_addr,
                   uint16_t cqid, uint16_t vector, uint16_t size,
                   uint16_t irq_enabled)
{
    PCIDevice *pci = PCI_DEVICE(n);

    cq->ctrl = n;
    cq->cqid = cqid;
    cq->size = size;
    cq->dma_addr = dma_addr;
    cq->vector = vector;
    cq->irq_enabled = irq_enabled;
    cq->head = 0;
    cq->tail = 0;
    cq->phase = 1;  /* Start with phase bit set */

    /* Initialize lists */
    QTAILQ_INIT(&cq->sq_list);
    QTAILQ_INIT(&cq->req_list);

    /* Create bottom-half for completion posting */
    cq->bh = qemu_bh_new(accel_post_cqes, cq);

    /* Enable MSI-X vector if interrupts enabled */
    if (msix_enabled(pci) && irq_enabled) {
        msix_vector_use(pci, vector);
    }
}

/**
 * accel_free_sq - Free submission queue resources
 * @sq: Submission queue to free
 * @n: Device state
 */
void accel_free_sq(AccelSQueue *sq, PCIeAccel *n)
{
    /* Cancel pending bottom-half */
    qemu_bh_cancel(sq->bh);
    qemu_bh_delete(sq->bh);

    /* Unlink from completion queue */
    if (n->cq[sq->cqid]) {
        QTAILQ_REMOVE(&n->cq[sq->cqid]->sq_list, sq, entry);
    }

    /* Free request pool */
    g_free(sq->io_req);

    g_free(sq);
}

/**
 * accel_free_cq - Free completion queue resources
 * @cq: Completion queue to free
 * @n: Device state
 */
void accel_free_cq(AccelCQueue *cq, PCIeAccel *n)
{
    PCIDevice *pci = PCI_DEVICE(n);

    /* Cancel pending bottom-half */
    qemu_bh_cancel(cq->bh);
    qemu_bh_delete(cq->bh);

    /* Disable MSI-X vector if enabled */
    if (msix_enabled(pci) && cq->irq_enabled) {
        msix_vector_unuse(pci, cq->vector);
    }

    g_free(cq);
}

/*
 * ===== Device Lifecycle Functions =====
 */

/**
 * pcie_accel_reset - Reset device to initial state
 * @dev: Device state
 */
void pcie_accel_reset(DeviceState *dev)
{
    PCIeAccel *n = PCIE_ACCEL(dev);

    /* Reset controller status */
    n->bar.csts = 0;
    n->bar.cc = (ACCEL_SQES << ACCEL_CC_IOSQES_SHIFT) |
                (ACCEL_CQES << ACCEL_CC_IOCQES_SHIFT);

    /* Reset admin queues */
    n->bar.asq = 0;
    n->bar.acq = 0;

    /* Clear admin queue links */
    n->sq[0] = NULL;
    n->cq[0] = NULL;

    /* Reset interrupt coalescing */
    n->bar.intcoal = 0;
    n->intcoal_thresh = 0;
    n->intcoal_time = 0;

    /* Reset statistics */
    memset(&n->stats, 0, sizeof(n->stats));

    /* Clear P2P peers */
    n->p2p.num_peers = 0;
    QTAILQ_INIT(&n->p2p.peer_list);

    /* Reset P2P ring buffers */
    accel_p2p_ring_reset(n);
}

/**
 * pcie_accel_realize - Initialize and realize the device
 * @pci_dev: PCI device
 * @errp: Error pointer
 */
void pcie_accel_realize(PCIDevice *pci_dev, Error **errp)
{
    PCIeAccel *n = PCIE_ACCEL(pci_dev);
    Error *local_err = NULL;
    int ret;

    /* Assign sequential device ID */
    static uint16_t next_dev_id;
    n->dev_id = next_dev_id++;

    /* Initialize PCIe endpoint capability */
    ret = pcie_endpoint_cap_init(pci_dev, 0x80);
    if (ret < 0) {
        error_setg(errp, "Failed to initialize PCIe endpoint capability");
        return;
    }

    /* Always add AER capability to initialize extended config space chain */
    pcie_aer_init(pci_dev, PCI_ERR_VER, 0x100, PCI_ERR_SIZEOF, NULL);

    /* Optionally add serial number capability */
    if (n->serial) {
        pcie_dev_ser_num_init(pci_dev, 0x150, 0x1234567890ABCDEF);
    }

    /* Initialize BAR0 (MMIO registers + doorbells) */
    memory_region_init_io(&n->bar0, OBJECT(n), &accel_mmio_ops, n,
                          "pcie-accel-bar0", ACCEL_BAR0_SIZE);
    pci_register_bar(pci_dev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &n->bar0);

    /* Initialize BAR2 (CMB - Controller Memory Buffer) */
    memory_region_init_ram(&n->cmb, OBJECT(n), "pcie-accel-cmb",
                           ACCEL_CMB_SIZE, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    pci_register_bar(pci_dev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH,
                     &n->cmb);

    /* Initialize BAR4 (MSI-X) */
    memory_region_init(&n->msix_bar, OBJECT(n), "pcie-accel-msix",
                       ACCEL_BAR4_SIZE);
    pci_register_bar(pci_dev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_32,
                     &n->msix_bar);

    ret = msix_init(pci_dev, n->max_ioqpairs + 1,
                    &n->msix_bar, 4, ACCEL_MSIX_TABLE_OFFSET,
                    &n->msix_bar, 4, ACCEL_MSIX_PBA_OFFSET,
                    0x00, &local_err);
    if (ret < 0) {
        error_propagate(errp, local_err);
        return;
    }

    /* Enable MSI-X vector 0 for admin queue */
    msix_vector_use(pci_dev, 0);

    /* Initialize DPA memory from backend (device-internal, not BAR-mapped) */
    if (n->dpa_memdev) {
        n->dpa_mr = host_memory_backend_get_memory(n->dpa_memdev);
        if (!n->dpa_mr) {
            error_setg(errp, "Failed to get DPA memory from backend");
            return;
        }
    }

    /* Initialize capability register */
    n->bar.cap = (1ULL << ACCEL_CAP_P2P_SHIFT) |          /* P2P MMIO queues */
                 ((uint64_t)n->sva.enabled << ACCEL_CAP_SVA_SHIFT) | /* PASID/SVA */
                 (1ULL << ACCEL_CAP_PRPL_SHIFT) |          /* PRPL DMA */
                 (1ULL << ACCEL_CAP_SGL_SHIFT) |           /* SGL DMA */
                 (0x8ULL << ACCEL_CAP_P2P_CH_BS_SHIFT) |  /* 2^8 x 4KB P2P channel buf */
                 ((uint64_t)ACCEL_SQES << ACCEL_CAP_SQS_SHIFT) |   /* 64B SQE */
                 ((uint64_t)ACCEL_CQES << ACCEL_CAP_CQS_SHIFT) |   /* 16B CQE */
                 (12ULL << ACCEL_CAP_DEPTH_SHIFT) |        /* 2^12=4096 entries */
                 (8ULL << ACCEL_CAP_MAXQ_SHIFT) |          /* 2^8=256 pairs */
                 (4ULL << ACCEL_CAP_MAXR_SHIFT);           /* 2^4=16 ring bufs */

    /* Initialize P2P configuration */
    n->bar.p2pcfg = (n->p2p.max_peers << ACCEL_P2PCFG_MAX_DEVICES_SHIFT) |
                    (n->p2p.max_xfers_per_peer << ACCEL_P2PCFG_MAX_XFERS_SHIFT);

    /* Initialize CMB BAR register (CMB is in BAR2) */
    n->bar.cmbbar = 2;
    n->bar.cmbsz = ACCEL_CMB_SIZE;

    /* Initialize P2P Ring configuration */
    uint32_t ring_size_4k = ACCEL_RING_SIZE / 4096;
    n->p2p.p2rcfg = (ACCEL_P2R_MAX_SLOTS << ACCEL_P2RCFG_SLOTS_SHIFT) |
                    (ring_size_4k << ACCEL_P2RCFG_RING_SIZE_SHIFT);

    /* Initialize page size */
    n->page_size = 4096;
    n->page_bits = 12;

    /* Allocate queue arrays */
    n->sq = g_new0(AccelSQueue *, n->max_ioqpairs + 1);
    n->cq = g_new0(AccelCQueue *, n->max_ioqpairs + 1);

    /* Initialize P2P peer list */
    QTAILQ_INIT(&n->p2p.peer_list);

    /* Initialize PASID/SVA if enabled */
    if (n->sva.enabled) {
        pcie_pasid_init(pci_dev, 0x150, n->sva.pasid_width, false, false);
        pcie_ats_init(pci_dev, 0x170, false);

        /* Allocate per-PASID address space array */
        int max_pasid = 1 << n->sva.pasid_width;
        n->sva.pasid_as = g_new0(AddressSpace *, max_pasid);
    }

    /* Initialize DMA ops (may be overridden by CXL variant) */
    n->dma_ops.read = accel_dma_read_safe;
    n->dma_ops.write = accel_dma_write_safe;

    /* Reset to initialize registers */
    pcie_accel_reset(DEVICE(n));
}

/**
 * pcie_accel_exit - Cleanup and exit the device
 * @pci_dev: PCI device
 */
void pcie_accel_exit(PCIDevice *pci_dev)
{
    PCIeAccel *n = PCIE_ACCEL(pci_dev);

    /* Cleanup P2P ring buffers */
    accel_p2p_ring_cleanup(n);

    /* Free all I/O queues */
    for (int i = 1; i <= n->max_ioqpairs; i++) {
        if (n->sq[i]) {
            accel_free_sq(n->sq[i], n);
        }
        if (n->cq[i]) {
            accel_free_cq(n->cq[i], n);
        }
    }

    /* Free admin queues */
    if (n->admin_sq.bh) {
        qemu_bh_delete(n->admin_sq.bh);
        g_free(n->admin_sq.io_req);
    }
    if (n->admin_cq.bh) {
        qemu_bh_delete(n->admin_cq.bh);
    }

    /* Free queue arrays */
    g_free(n->sq);
    g_free(n->cq);

    /* Cleanup PASID/SVA */
    if (n->sva.pasid_as) {
        g_free(n->sva.pasid_as);
    }

    /* Cleanup MSI-X */
    msix_uninit(pci_dev, &n->msix_bar, &n->msix_bar);
}

/*
 * ===== Device Properties and Class Definition =====
 */

static const Property pcie_accel_props[] = {
    DEFINE_PROP_STRING("serial", PCIeAccel, serial),
    DEFINE_PROP_UINT32("max_ioqpairs", PCIeAccel, max_ioqpairs, 64),
    DEFINE_PROP_UINT8("p2p_max_peers", PCIeAccel, p2p.max_peers, 32),
    DEFINE_PROP_UINT8("p2p_max_xfers", PCIeAccel, p2p.max_xfers_per_peer, 64),
    DEFINE_PROP_BOOL("sva_enable", PCIeAccel, sva.enabled, false),
    DEFINE_PROP_UINT8("pasid_width", PCIeAccel, sva.pasid_width, 16),
    DEFINE_PROP_LINK("dpa_memdev", PCIeAccel, dpa_memdev,
                     TYPE_MEMORY_BACKEND, HostMemoryBackend *),
};

static void pcie_accel_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = pcie_accel_realize;
    pc->exit = pcie_accel_exit;
    pc->vendor_id = ACCEL_PCIE_VENDOR_ID;
    pc->device_id = ACCEL_PCIE_DEVICE_ID;
    pc->revision = ACCEL_PCIE_REVISION;
    pc->class_id = ACCEL_PCIE_CLASS;

    device_class_set_props(dc, pcie_accel_props);
    dc->desc = "PCIe Accelerator Device";
    device_class_set_legacy_reset(dc, pcie_accel_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pcie_accel_info = {
    .name = TYPE_PCIE_ACCEL,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PCIeAccel),
    .class_init = pcie_accel_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void pcie_accel_register_types(void)
{
    type_register_static(&pcie_accel_info);
}

type_init(pcie_accel_register_types)
