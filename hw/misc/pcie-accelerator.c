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
#include "trace.h"

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
            trace_pcie_accel_irq_assert(cq->cqid, cq->vector);
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

    trace_pcie_accel_irq_deassert(cq->cqid);
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
        req->cqe.sq_id = cpu_to_le16(sq->sqid);
        req->cqe.sq_head = cpu_to_le16(sq->head);

        /* Set status with phase bit */
        req->cqe.status = cpu_to_le16(ACCEL_CQE_BUILD_STATUS(req->status,
                                                               cq->phase));

        /* Write CQE to host memory */
        addr = cq->dma_addr + (cq->tail << ACCEL_CQES);
        status = accel_dma_write_safe(n, addr, &req->cqe, sizeof(req->cqe));

        if (status != ACCEL_SC_SUCCESS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Failed to write CQE at 0x%" PRIx64 "\n",
                          addr);
            accel_set_ctrl_fatal(n);
            break;
        }

        trace_pcie_accel_post_cqe(cq->cqid, sq->sqid, req->cqe.cid,
                                   req->cqe.status);

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: Posted CQE: cqid=%u sqid=%u cid=%u status=0x%x addr=0x%" PRIx64 "\n",
                      cq->cqid, sq->sqid, le16_to_cpu(req->cqe.cid),
                      le16_to_cpu(req->cqe.status), addr);

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
    if (cmd->opcode > ACCEL_CMD_P2P_READ) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Invalid opcode 0x%x\n", cmd->opcode);
        return ACCEL_SC_INVALID_OPCODE;
    }

    /* Validate data pointers for commands that need them */
    switch (cmd->opcode) {
    case ACCEL_CMD_LOOPBACK:
    case ACCEL_CMD_P2P_WRITE:
    case ACCEL_CMD_P2P_READ:
        if (cmd->prp1 == 0) {
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
    if (cmd->flags & ACCEL_CMD_FLAG_PASID_ENABLE) {
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
    uint64_t prp1 = le64_to_cpu(cmd->prp1);
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
        req->cqe.result = cpu_to_le32(length);
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
 * accel_io_cmd - Dispatch I/O commands
 * @n: Device state
 * @req: Request structure
 *
 * Returns: Status code or ACCEL_NO_COMPLETE for async commands
 */
uint16_t accel_io_cmd(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;

    trace_pcie_accel_cmd_dispatch(req->sq->sqid, le16_to_cpu(cmd->cid),
                                   cmd->opcode);

    switch (cmd->opcode) {
    case ACCEL_CMD_LOOPBACK:
        return accel_cmd_loopback(n, req);

    case ACCEL_CMD_P2P_WRITE:
        return accel_cmd_p2p_write(n, req);

    case ACCEL_CMD_P2P_READ:
        return accel_cmd_p2p_read(n, req);

    default:
        trace_pcie_accel_err_invalid_cmd(req->sq->sqid, cmd->opcode);
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
    uint64_t prp1 = le64_to_cpu(cmd->prp1);
    uint16_t status;
    uint8_t buf[4096] = {0};

    /*
     * Identify structure (simplified):
     * Offset 0-3:   Version (major.minor.tertiary)
     * Offset 4-7:   Max I/O queue pairs
     * Offset 8-11:  Max queue entries
     * Offset 12-15: P2P max peers
     * Offset 16-19: P2P max xfers
     * Offset 20-23: PASID width (0 if disabled)
     */

    /* Version (1.0.0) */
    *(uint32_t *)(buf + 0) = cpu_to_le32(0x00010000);

    /* Queue capabilities */
    *(uint32_t *)(buf + 4) = cpu_to_le32(n->max_ioqpairs);
    *(uint32_t *)(buf + 8) = cpu_to_le32(ACCEL_MAX_QUEUE_ENTRIES);

    /* P2P capabilities */
    *(uint32_t *)(buf + 12) = cpu_to_le32(n->p2p.max_peers);
    *(uint32_t *)(buf + 16) = cpu_to_le32(n->p2p.max_xfers_per_peer);

    /* PASID/SVA capabilities */
    *(uint32_t *)(buf + 20) = cpu_to_le32(n->sva.enabled ? n->sva.pasid_width : 0);

    /* Write to host memory */
    status = accel_dma_write_safe(n, prp1, buf, sizeof(buf));

    return status;
}

/**
 * accel_cmd_create_cq - Create I/O completion queue
 * @n: Device state
 * @req: Request structure
 *
 * Creates a completion queue with the specified parameters.
 */
uint16_t accel_cmd_create_cq(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t cqid = le32_to_cpu(cmd->dw.admin.cdw10) & 0xFFFF;
    uint16_t qsize = (le32_to_cpu(cmd->dw.admin.cdw10) >> 16) & 0xFFFF;
    uint16_t vector = le32_to_cpu(cmd->dw.admin.cdw11) & 0xFFFF;
    uint16_t irq_en = (le32_to_cpu(cmd->dw.admin.cdw11) >> 16) & 0x1;
    uint64_t prp1 = le64_to_cpu(cmd->prp1);
    AccelCQueue *cq;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: CREATE_CQ: cqid=%u qsize=%u prp1=0x%" PRIx64
                  " vector=%u irq_en=%u\n",
                  cqid, qsize, prp1, vector, irq_en);

    /* Validate CQ ID */
    if (cqid == 0 || cqid > n->max_ioqpairs) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: CREATE_CQ invalid cqid %u (max=%u)\n",
                      cqid, n->max_ioqpairs);
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    if (n->cq[cqid] != NULL) {
        return ACCEL_SC_QUEUE_ALREADY_EXISTS;
    }

    /* Validate queue size */
    if (qsize < 1 || qsize > ACCEL_MAX_QUEUE_ENTRIES) {
        return ACCEL_SC_INVALID_QUEUE_SIZE;
    }

    /* Validate address alignment */
    if (prp1 & (n->page_size - 1)) {
        return ACCEL_SC_INVALID_QUEUE_ADDR;
    }

    /* Validate MSI-X vector */
    if (irq_en && vector >= (n->max_ioqpairs + 1)) {
        return ACCEL_SC_INVALID_IRQ_VECTOR;
    }

    /* Allocate and initialize CQ */
    cq = g_malloc0(sizeof(AccelCQueue));
    accel_init_cq(cq, n, prp1, cqid, vector, qsize + 1, irq_en);

    n->cq[cqid] = cq;
    n->conf_ioqpairs = MAX(n->conf_ioqpairs, cqid);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: CREATE_CQ success: cqid=%u size=%u\n",
                  cqid, qsize + 1);

    /* Update device status */
    n->bar.devstat = (n->bar.devstat & ~(0xFF << ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT)) |
                     ((n->conf_ioqpairs & 0xFF) << ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_cmd_create_sq - Create I/O submission queue
 * @n: Device state
 * @req: Request structure
 *
 * Creates a submission queue associated with a completion queue.
 */
uint16_t accel_cmd_create_sq(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t sqid = le32_to_cpu(cmd->dw.admin.cdw10) & 0xFFFF;
    uint16_t qsize = (le32_to_cpu(cmd->dw.admin.cdw10) >> 16) & 0xFFFF;
    uint16_t cqid = le32_to_cpu(cmd->dw.admin.cdw11) & 0xFFFF;
    uint64_t prp1 = le64_to_cpu(cmd->prp1);
    AccelSQueue *sq;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: CREATE_SQ: sqid=%u qsize=%u cqid=%u prp1=0x%" PRIx64 "\n",
                  sqid, qsize, cqid, prp1);

    /* Validate SQ ID */
    if (sqid == 0 || sqid > n->max_ioqpairs) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: CREATE_SQ invalid sqid %u (max=%u)\n",
                      sqid, n->max_ioqpairs);
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    if (n->sq[sqid] != NULL) {
        return ACCEL_SC_QUEUE_ALREADY_EXISTS;
    }

    /* Validate CQ ID */
    if (!accel_check_cqid(n, cqid)) {
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    /* Validate queue size */
    if (qsize < 1 || qsize > ACCEL_MAX_QUEUE_ENTRIES) {
        return ACCEL_SC_INVALID_QUEUE_SIZE;
    }

    /* Validate address alignment */
    if (prp1 & (n->page_size - 1)) {
        return ACCEL_SC_INVALID_QUEUE_ADDR;
    }

    /* Allocate and initialize SQ */
    sq = g_malloc0(sizeof(AccelSQueue));
    accel_init_sq(sq, n, prp1, sqid, cqid, qsize + 1);

    n->sq[sqid] = sq;

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: CREATE_SQ success: sqid=%u size=%u cqid=%u\n",
                  sqid, qsize + 1, cqid);

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_cmd_delete_cq - Delete I/O completion queue
 * @n: Device state
 * @req: Request structure
 */
uint16_t accel_cmd_delete_cq(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t cqid = le32_to_cpu(cmd->dw.admin.cdw10) & 0xFFFF;
    AccelCQueue *cq;

    if (!accel_check_cqid(n, cqid) || cqid == 0) {
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    cq = n->cq[cqid];

    /* Check if any SQs are still using this CQ */
    if (!QTAILQ_EMPTY(&cq->sq_list)) {
        return ACCEL_SC_INVALID_QUEUE_ID;  /* CQ still in use */
    }

    accel_free_cq(cq, n);
    n->cq[cqid] = NULL;

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_cmd_delete_sq - Delete I/O submission queue
 * @n: Device state
 * @req: Request structure
 */
uint16_t accel_cmd_delete_sq(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint16_t sqid = le32_to_cpu(cmd->dw.admin.cdw10) & 0xFFFF;
    AccelSQueue *sq;

    if (!accel_check_sqid(n, sqid) || sqid == 0) {
        return ACCEL_SC_INVALID_QUEUE_ID;
    }

    sq = n->sq[sqid];

    /* Abort any in-flight requests */
    AccelRequest *req_iter, *next;
    QTAILQ_FOREACH_SAFE(req_iter, &sq->out_req_list, entry, next) {
        req_iter->status = ACCEL_SC_CMD_ABORT_SQID;
        accel_enqueue_req_completion(n->cq[sq->cqid], req_iter);
    }

    accel_free_sq(sq, n);
    n->sq[sqid] = NULL;

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

    trace_pcie_accel_cmd_dispatch(0, le16_to_cpu(cmd->cid), cmd->opcode);

    switch (cmd->opcode) {
    case ACCEL_ADM_CMD_DELETE_SQ:
        return accel_cmd_delete_sq(n, req);

    case ACCEL_ADM_CMD_CREATE_SQ:
        return accel_cmd_create_sq(n, req);

    case ACCEL_ADM_CMD_DELETE_CQ:
        return accel_cmd_delete_cq(n, req);

    case ACCEL_ADM_CMD_CREATE_CQ:
        return accel_cmd_create_cq(n, req);

    case ACCEL_ADM_CMD_IDENTIFY:
        return accel_cmd_identify(n, req);

    case ACCEL_ADM_CMD_P2P_SETUP:
        return accel_cmd_p2p_setup(n, req);

    case ACCEL_ADM_CMD_P2P_RING_SETUP:
        return accel_cmd_p2p_ring_setup(n, req);

    case ACCEL_ADM_CMD_P2P_RING_TEARDOWN:
        return accel_cmd_p2p_ring_teardown(n, req);

    default:
        trace_pcie_accel_err_invalid_cmd(0, cmd->opcode);
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

    trace_pcie_accel_process_sq(sq->sqid, sq->head, sq->tail);

    qemu_log_mask(LOG_UNIMP,
                  "pcie-accel: process_sq called: sqid=%u head=%u tail=%u "
                  "sq_empty=%d req_list_empty=%d\n",
                  sq->sqid, sq->head, sq->tail,
                  accel_sq_empty(sq), QTAILQ_EMPTY(&sq->req_list));

    /* Process commands until queue is empty or no requests available */
    while (!accel_sq_empty(sq) && !QTAILQ_EMPTY(&sq->req_list)) {
        /* Fetch command from host memory */
        addr = sq->dma_addr + (sq->head << ACCEL_SQES);
        status = accel_dma_read_safe(n, addr, &cmd, sizeof(cmd));

        if (status != ACCEL_SC_SUCCESS) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "pcie-accel: Failed to read command from SQ %u at 0x%"
                          PRIx64 "\n", sq->sqid, addr);
            accel_set_ctrl_fatal(n);
            break;
        }

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

        trace_pcie_accel_doorbell_cq(qid, new_head);

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

        trace_pcie_accel_doorbell_sq(qid, new_tail);

        qemu_log_mask(LOG_UNIMP,
                      "pcie-accel: SQ %u doorbell: new_tail=%u (head=%u size=%u)\n",
                      qid, new_tail, sq->head, sq->size);

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

    trace_pcie_accel_mmio_read(addr, val, size);

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

    trace_pcie_accel_mmio_write(addr, data, size);

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

    /* Initialize capability register */
    n->bar.cap = (1ULL << ACCEL_CAP_P2P_SHIFT) |          /* P2P MMIO queues */
                 (1ULL << ACCEL_CAP_SVA_SHIFT) |           /* PASID/SVA */
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
