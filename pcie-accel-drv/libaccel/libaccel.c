/* SPDX-License-Identifier: MIT */
/*
 * PCIe Accelerator Device - User Mode Library with io_uring
 *
 * Copyright (C) 2026
 *
 * This implementation uses liburing for high-performance async I/O.
 * Commands are submitted via io_uring IORING_OP_URING_CMD operations
 * and completed asynchronously via CQEs.
 *
 * Architecture:
 * - Each accel_device has its own io_uring instance
 * - Async operations are submitted as SQEs with URING_CMD opcode
 * - The kernel driver's uring_cmd handler processes these
 * - Completions arrive as io_uring CQEs
 * - User data links CQEs back to async tokens
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <liburing.h>

#include "libaccel.h"

/* Default io_uring parameters */
#define DEFAULT_URING_ENTRIES   256
#define MAX_PENDING_OPS         4096

/*
 * ===== io_uring Command Structure =====
 *
 * This must match the kernel's struct accel_uring_cmd.
 * It's embedded in the SQE's cmd field (80 bytes).
 */
struct accel_uring_cmd {
    uint8_t  op;              /* accel_uring_cmd_op */
    uint8_t  flags;
    uint16_t qid;
    uint32_t timeout_ms;

    union {
        /* ACCEL_URING_CMD_SUBMIT */
        struct {
            struct accel_cmd cmd;
        } submit;

        /* ACCEL_URING_CMD_CREATE_QUEUE */
        struct {
            uint16_t sq_size;
            uint16_t cq_size;
            uint16_t flags;
            uint16_t rsvd;
        } create_queue;

        /* ACCEL_URING_CMD_DELETE_QUEUE */
        struct {
            uint16_t qid;
            uint16_t rsvd[3];
        } delete_queue;

        /* ACCEL_URING_CMD_SETUP_P2P */
        struct {
            uint16_t peer_bdf;
            uint16_t flags;
            uint32_t rsvd;
        } setup_p2p;
    };
} __attribute__((packed));

/*
 * ===== Internal Device Structure =====
 */
struct accel_device {
    int fd;                           /* Device file descriptor */
    struct io_uring ring;             /* io_uring instance */
    int ring_fd;                      /* io_uring fd */

    /* Pending operation tracking */
    uint64_t next_user_data;          /* Counter for unique user_data */
    int pending_count;                /* Number of pending operations */
    bool batch_mode;                  /* True if batching enabled */

    /* Statistics */
    uint64_t total_submitted;
    uint64_t total_completed;
};

/*
 * ===== Error Messages =====
 */
static const char *error_messages[] = {
    [0] = "Success",
    [1] = "Failed to open device",
    [2] = "IOCTL failed",
    [3] = "Out of memory",
    [4] = "Invalid argument",
    [5] = "Operation timed out",
    [6] = "I/O error",
    [7] = "Device not found",
    [8] = "Device busy",
    [9] = "io_uring setup failed",
    [10] = "Submission failed",
    [11] = "Operation canceled",
};

/**
 * accel_strerror - Get error message for error code
 */
const char *accel_strerror(int err)
{
    if (err >= 0)
        return error_messages[0];

    int idx = -err;
    if (idx < (int)(sizeof(error_messages) / sizeof(error_messages[0])) &&
        error_messages[idx])
        return error_messages[idx];

    return "Unknown error";
}

/*
 * ===== Device Management =====
 */

/**
 * accel_open_with_params - Open device with custom io_uring parameters
 */
struct accel_device *accel_open_with_params(const char *dev_path,
                                            unsigned int sq_entries,
                                            unsigned int flags)
{
    struct accel_device *dev;
    int ret;

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;

    /* Open device file */
    dev->fd = open(dev_path, O_RDWR);
    if (dev->fd < 0) {
        free(dev);
        return NULL;
    }

    /* Set up io_uring */
    if (sq_entries == 0)
        sq_entries = DEFAULT_URING_ENTRIES;

    struct io_uring_params params = {0};
    params.flags = flags;

    ret = io_uring_queue_init_params(sq_entries, &dev->ring, &params);
    if (ret < 0) {
        close(dev->fd);
        free(dev);
        return NULL;
    }

    dev->ring_fd = dev->ring.ring_fd;
    dev->next_user_data = 1;  /* Start at 1, 0 reserved for sync ops */

    return dev;
}

/**
 * accel_open - Open device with default parameters
 */
struct accel_device *accel_open(const char *dev_path)
{
    return accel_open_with_params(dev_path, 0, 0);
}

/**
 * accel_close - Close device handle
 */
void accel_close(struct accel_device *dev)
{
    if (!dev)
        return;

    /* Wait for any pending operations */
    while (dev->pending_count > 0) {
        struct io_uring_cqe *cqe;
        io_uring_wait_cqe(&dev->ring, &cqe);
        io_uring_cqe_seen(&dev->ring, cqe);
        dev->pending_count--;
    }

    io_uring_queue_exit(&dev->ring);
    close(dev->fd);
    free(dev);
}

/**
 * accel_get_uring_fd - Get io_uring file descriptor
 */
int accel_get_uring_fd(struct accel_device *dev)
{
    return dev ? dev->ring_fd : -1;
}

/**
 * accel_get_device_fd - Get device file descriptor
 */
int accel_get_device_fd(struct accel_device *dev)
{
    return dev ? dev->fd : -1;
}

/*
 * ===== Internal Helper Functions =====
 */

/**
 * prepare_uring_cmd - Prepare an io_uring SQE for uring_cmd
 */
/*
 * io_uring_prep_uring_cmd helper - manually set up IORING_OP_URING_CMD
 * since older liburing versions don't have this function.
 */
static inline void prep_uring_cmd(struct io_uring_sqe *sqe, int fd,
                                  struct accel_uring_cmd *ucmd)
{
    io_uring_prep_rw(IORING_OP_URING_CMD, sqe, fd, NULL, 0, 0);
    memcpy(sqe->cmd, ucmd, sizeof(*ucmd));
}

static struct io_uring_sqe *prepare_uring_cmd(struct accel_device *dev,
                                              struct accel_uring_cmd *ucmd,
                                              uint64_t user_data)
{
    struct io_uring_sqe *sqe;

    sqe = io_uring_get_sqe(&dev->ring);
    if (!sqe)
        return NULL;

    /* Set up URING_CMD operation */
    prep_uring_cmd(sqe, dev->fd, ucmd);
    sqe->user_data = user_data;

    return sqe;
}

/**
 * submit_and_wait_sync - Submit and wait for single sync operation
 */
static int submit_and_wait_sync(struct accel_device *dev)
{
    struct io_uring_cqe *cqe;
    int ret;

    /* Submit */
    ret = io_uring_submit(&dev->ring);
    if (ret < 0)
        return ACCEL_ERR_SUBMIT;

    /* Wait for completion */
    ret = io_uring_wait_cqe(&dev->ring, &cqe);
    if (ret < 0)
        return ACCEL_ERR_IO;

    /* Get result */
    ret = cqe->res;
    io_uring_cqe_seen(&dev->ring, cqe);

    dev->total_completed++;

    if (ret < 0)
        return ACCEL_ERR_IO;

    return ret;
}

/*
 * ===== Queue Management =====
 */

/**
 * accel_create_queue - Create I/O queue pair
 */
int accel_create_queue(struct accel_device *dev, uint16_t qid,
                       uint16_t sq_size, uint16_t cq_size)
{
    struct accel_uring_cmd ucmd = {0};
    struct io_uring_sqe *sqe;

    if (!dev)
        return ACCEL_ERR_INVAL;

    ucmd.op = ACCEL_URING_CMD_CREATE_QUEUE;
    ucmd.qid = qid;
    ucmd.create_queue.sq_size = sq_size;
    ucmd.create_queue.cq_size = cq_size;

    sqe = prepare_uring_cmd(dev, &ucmd, 0);
    if (!sqe)
        return ACCEL_ERR_BUSY;

    dev->total_submitted++;
    return submit_and_wait_sync(dev);
}

/**
 * accel_delete_queue - Delete I/O queue pair
 */
int accel_delete_queue(struct accel_device *dev, uint16_t qid)
{
    struct accel_uring_cmd ucmd = {0};
    struct io_uring_sqe *sqe;

    if (!dev)
        return ACCEL_ERR_INVAL;

    ucmd.op = ACCEL_URING_CMD_DELETE_QUEUE;
    ucmd.qid = qid;
    ucmd.delete_queue.qid = qid;

    sqe = prepare_uring_cmd(dev, &ucmd, 0);
    if (!sqe)
        return ACCEL_ERR_BUSY;

    dev->total_submitted++;
    return submit_and_wait_sync(dev);
}

/*
 * ===== P2P Operations =====
 */

/**
 * accel_setup_p2p_peer - Register P2P peer device
 */
int accel_setup_p2p_peer(struct accel_device *dev, uint16_t peer_bdf)
{
    struct accel_uring_cmd ucmd = {0};
    struct io_uring_sqe *sqe;

    if (!dev)
        return ACCEL_ERR_INVAL;

    ucmd.op = ACCEL_URING_CMD_SETUP_P2P;
    ucmd.setup_p2p.peer_bdf = peer_bdf;

    sqe = prepare_uring_cmd(dev, &ucmd, 0);
    if (!sqe)
        return ACCEL_ERR_BUSY;

    dev->total_submitted++;
    return submit_and_wait_sync(dev);
}

/*
 * ===== Device Information =====
 */

/**
 * accel_identify - Get device identification
 *
 * Note: This uses IOCTL for simplicity since it's a management operation.
 */
int accel_identify(struct accel_device *dev, struct accel_identify *id)
{
    struct accel_uring_cmd ucmd = {0};
    struct io_uring_sqe *sqe;
    int ret;

    if (!dev || !id)
        return ACCEL_ERR_INVAL;

    ucmd.op = ACCEL_URING_CMD_ADMIN;
    ucmd.qid = 0;
    ucmd.submit.cmd.opcode = ACCEL_ADM_CMD_IDENTIFY;

    sqe = prepare_uring_cmd(dev, &ucmd, 0);
    if (!sqe)
        return ACCEL_ERR_BUSY;

    dev->total_submitted++;
    ret = submit_and_wait_sync(dev);
    if (ret < 0)
        return ret;

    /* Fill in identify data (simplified - in production would read from device) */
    memset(id, 0, sizeof(*id));
    id->version = 0x020000;  /* v2.0.0 */
    id->max_queues = 64;
    id->max_queue_size = 4096;
    id->p2p_max_peers = 32;
    id->p2p_max_xfers = 256;
    id->pasid_width = 20;

    return ACCEL_SUCCESS;
}

/**
 * accel_get_stats - Get device statistics
 */
int accel_get_stats(struct accel_device *dev, struct accel_stats *stats)
{
    struct accel_uring_cmd ucmd = {0};
    struct io_uring_sqe *sqe;
    int ret;

    if (!dev || !stats)
        return ACCEL_ERR_INVAL;

    ucmd.op = ACCEL_URING_CMD_GET_STATS;

    sqe = prepare_uring_cmd(dev, &ucmd, 0);
    if (!sqe)
        return ACCEL_ERR_BUSY;

    dev->total_submitted++;
    ret = submit_and_wait_sync(dev);

    /* Decode packed stats from result */
    memset(stats, 0, sizeof(*stats));
    if (ret >= 0) {
        stats->cmd_submitted = ret & 0xFFFF;
        stats->uring_submissions = (ret >> 16) & 0xFFFF;
    }

    /* Add local stats */
    stats->uring_submissions = dev->total_submitted;
    stats->uring_completions = dev->total_completed;

    return ACCEL_SUCCESS;
}

/*
 * ===== Synchronous Command Submission =====
 */

/**
 * accel_submit_cmd - Submit command synchronously
 */
int accel_submit_cmd(struct accel_device *dev, uint16_t qid,
                     struct accel_cmd *cmd, struct accel_cqe *cqe,
                     uint32_t timeout_ms)
{
    struct accel_uring_cmd ucmd = {0};
    struct io_uring_sqe *sqe;
    int ret;

    if (!dev || !cmd)
        return ACCEL_ERR_INVAL;

    ucmd.op = ACCEL_URING_CMD_SUBMIT;
    ucmd.qid = qid;
    ucmd.timeout_ms = timeout_ms ? timeout_ms : 5000;
    memcpy(&ucmd.submit.cmd, cmd, sizeof(*cmd));

    sqe = prepare_uring_cmd(dev, &ucmd, 0);
    if (!sqe)
        return ACCEL_ERR_BUSY;

    dev->total_submitted++;
    ret = submit_and_wait_sync(dev);

    if (cqe) {
        memset(cqe, 0, sizeof(*cqe));
        cqe->result = (ret >= 0) ? ret : 0;
        cqe->status = (ret >= 0) ? 0 : 1;
    }

    return (ret >= 0) ? ACCEL_SUCCESS : ACCEL_ERR_IO;
}

/**
 * accel_loopback - Synchronous loopback operation
 */
int accel_loopback(struct accel_device *dev, uint16_t qid,
                   void *data, uint32_t length, uint32_t pattern)
{
    struct accel_cmd cmd = {0};

    if (!dev || !data || length == 0)
        return ACCEL_ERR_INVAL;

    cmd.opcode = ACCEL_CMD_LOOPBACK;
    cmd.prp1 = (uint64_t)(uintptr_t)data;
    cmd.dw.loopback.length = length;
    cmd.dw.loopback.pattern = pattern;

    return accel_submit_cmd(dev, qid, &cmd, NULL, 5000);
}

/**
 * accel_p2p_write - Synchronous P2P write
 */
int accel_p2p_write(struct accel_device *dev, uint16_t qid,
                    uint16_t peer_bdf, const void *local_data,
                    uint64_t peer_addr, uint32_t length)
{
    struct accel_cmd cmd = {0};

    if (!dev || !local_data || length == 0)
        return ACCEL_ERR_INVAL;

    cmd.opcode = ACCEL_CMD_P2P_WRITE;
    cmd.prp1 = (uint64_t)(uintptr_t)local_data;
    cmd.dw.p2p.length = length;
    cmd.dw.p2p.peer_addr = peer_addr;
    cmd.dw.p2p.peer_bdf = peer_bdf;

    return accel_submit_cmd(dev, qid, &cmd, NULL, 5000);
}

/**
 * accel_p2p_read - Synchronous P2P read
 */
int accel_p2p_read(struct accel_device *dev, uint16_t qid,
                   uint16_t peer_bdf, void *local_data,
                   uint64_t peer_addr, uint32_t length)
{
    struct accel_cmd cmd = {0};

    if (!dev || !local_data || length == 0)
        return ACCEL_ERR_INVAL;

    cmd.opcode = ACCEL_CMD_P2P_READ;
    cmd.prp1 = (uint64_t)(uintptr_t)local_data;
    cmd.dw.p2p.length = length;
    cmd.dw.p2p.peer_addr = peer_addr;
    cmd.dw.p2p.peer_bdf = peer_bdf;

    return accel_submit_cmd(dev, qid, &cmd, NULL, 5000);
}

/*
 * ===== Asynchronous Command Submission =====
 */

/**
 * accel_async_submit_cmd - Submit command asynchronously
 */
int accel_async_submit_cmd(struct accel_device *dev, uint16_t qid,
                           struct accel_cmd *cmd,
                           struct accel_async_token *token)
{
    struct accel_uring_cmd ucmd = {0};
    struct io_uring_sqe *sqe;

    if (!dev || !cmd || !token)
        return ACCEL_ERR_INVAL;

    /* Assign unique user_data */
    token->user_data = dev->next_user_data++;
    token->status = -1;  /* Pending */

    ucmd.op = ACCEL_URING_CMD_SUBMIT;
    ucmd.qid = qid;
    ucmd.timeout_ms = 5000;
    memcpy(&ucmd.submit.cmd, cmd, sizeof(*cmd));

    sqe = prepare_uring_cmd(dev, &ucmd, token->user_data);
    if (!sqe)
        return ACCEL_ERR_BUSY;

    dev->pending_count++;
    dev->total_submitted++;

    /* Auto-submit if not in batch mode */
    if (!dev->batch_mode) {
        int ret = io_uring_submit(&dev->ring);
        if (ret < 0) {
            dev->pending_count--;
            return ACCEL_ERR_SUBMIT;
        }
    }

    return ACCEL_SUCCESS;
}

/**
 * accel_async_loopback - Async loopback operation
 */
int accel_async_loopback(struct accel_device *dev, uint16_t qid,
                         void *data, uint32_t length, uint32_t pattern,
                         struct accel_async_token *token)
{
    struct accel_cmd cmd = {0};

    if (!dev || !data || length == 0 || !token)
        return ACCEL_ERR_INVAL;

    cmd.opcode = ACCEL_CMD_LOOPBACK;
    cmd.prp1 = (uint64_t)(uintptr_t)data;
    cmd.dw.loopback.length = length;
    cmd.dw.loopback.pattern = pattern;

    token->buffer = data;
    token->length = length;

    return accel_async_submit_cmd(dev, qid, &cmd, token);
}

/**
 * accel_async_p2p_write - Async P2P write
 */
int accel_async_p2p_write(struct accel_device *dev, uint16_t qid,
                          uint16_t peer_bdf, const void *local_data,
                          uint64_t peer_addr, uint32_t length,
                          struct accel_async_token *token)
{
    struct accel_cmd cmd = {0};

    if (!dev || !local_data || length == 0 || !token)
        return ACCEL_ERR_INVAL;

    cmd.opcode = ACCEL_CMD_P2P_WRITE;
    cmd.prp1 = (uint64_t)(uintptr_t)local_data;
    cmd.dw.p2p.length = length;
    cmd.dw.p2p.peer_addr = peer_addr;
    cmd.dw.p2p.peer_bdf = peer_bdf;

    token->buffer = (void *)local_data;
    token->length = length;

    return accel_async_submit_cmd(dev, qid, &cmd, token);
}

/**
 * accel_async_p2p_read - Async P2P read
 */
int accel_async_p2p_read(struct accel_device *dev, uint16_t qid,
                         uint16_t peer_bdf, void *local_data,
                         uint64_t peer_addr, uint32_t length,
                         struct accel_async_token *token)
{
    struct accel_cmd cmd = {0};

    if (!dev || !local_data || length == 0 || !token)
        return ACCEL_ERR_INVAL;

    cmd.opcode = ACCEL_CMD_P2P_READ;
    cmd.prp1 = (uint64_t)(uintptr_t)local_data;
    cmd.dw.p2p.length = length;
    cmd.dw.p2p.peer_addr = peer_addr;
    cmd.dw.p2p.peer_bdf = peer_bdf;

    token->buffer = local_data;
    token->length = length;

    return accel_async_submit_cmd(dev, qid, &cmd, token);
}

/*
 * ===== Completion Handling =====
 */

/**
 * accel_submit_pending - Submit all pending operations
 */
int accel_submit_pending(struct accel_device *dev)
{
    if (!dev)
        return ACCEL_ERR_INVAL;

    return io_uring_submit(&dev->ring);
}

/**
 * accel_poll_completions - Poll for completions (non-blocking)
 */
int accel_poll_completions(struct accel_device *dev,
                           struct accel_async_token *tokens,
                           int max_tokens)
{
    struct io_uring_cqe *cqe;
    int completed = 0;
    unsigned head;

    if (!dev)
        return ACCEL_ERR_INVAL;

    /* Peek at available completions */
    io_uring_for_each_cqe(&dev->ring, head, cqe) {
        if (completed >= max_tokens)
            break;

        if (tokens) {
            tokens[completed].user_data = cqe->user_data;
            tokens[completed].status = (cqe->res >= 0) ? 0 : cqe->res;
            tokens[completed].result = (cqe->res >= 0) ? cqe->res : 0;
        }

        completed++;
        dev->pending_count--;
        dev->total_completed++;
    }

    /* Mark completions as seen */
    if (completed > 0)
        io_uring_cq_advance(&dev->ring, completed);

    return completed;
}

/**
 * accel_wait_completion - Wait for a specific completion
 */
int accel_wait_completion(struct accel_device *dev,
                          struct accel_async_token *token,
                          uint32_t *result)
{
    struct io_uring_cqe *cqe;
    int ret;

    if (!dev || !token)
        return ACCEL_ERR_INVAL;

    /* Keep waiting until we find our completion */
    while (1) {
        ret = io_uring_wait_cqe(&dev->ring, &cqe);
        if (ret < 0)
            return ACCEL_ERR_IO;

        dev->pending_count--;
        dev->total_completed++;

        if (cqe->user_data == token->user_data) {
            /* Found our completion */
            token->status = (cqe->res >= 0) ? 0 : cqe->res;
            token->result = (cqe->res >= 0) ? cqe->res : 0;

            if (result)
                *result = token->result;

            io_uring_cqe_seen(&dev->ring, cqe);

            return (token->status == 0) ? ACCEL_SUCCESS : ACCEL_ERR_IO;
        }

        /* Not our completion, keep looking */
        io_uring_cqe_seen(&dev->ring, cqe);
    }
}

/**
 * accel_wait_completions - Wait for multiple completions
 */
int accel_wait_completions(struct accel_device *dev,
                           struct accel_async_token *tokens,
                           int max_tokens, int min_complete,
                           uint32_t timeout_ms)
{
    struct io_uring_cqe *cqe;
    struct __kernel_timespec ts;
    int completed = 0;
    int ret;

    if (!dev)
        return ACCEL_ERR_INVAL;

    if (min_complete > max_tokens)
        min_complete = max_tokens;

    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;

    while (completed < min_complete) {
        if (timeout_ms > 0) {
            ret = io_uring_wait_cqe_timeout(&dev->ring, &cqe, &ts);
        } else {
            ret = io_uring_wait_cqe(&dev->ring, &cqe);
        }

        if (ret < 0) {
            if (ret == -ETIME)
                break;  /* Timeout - return what we have */
            return ACCEL_ERR_IO;
        }

        if (tokens && completed < max_tokens) {
            tokens[completed].user_data = cqe->user_data;
            tokens[completed].status = (cqe->res >= 0) ? 0 : cqe->res;
            tokens[completed].result = (cqe->res >= 0) ? cqe->res : 0;
        }

        completed++;
        dev->pending_count--;
        dev->total_completed++;
        io_uring_cqe_seen(&dev->ring, cqe);
    }

    return completed;
}

/**
 * accel_get_pending_count - Get number of pending operations
 */
int accel_get_pending_count(struct accel_device *dev)
{
    return dev ? dev->pending_count : 0;
}

/*
 * ===== Batch Operations =====
 */

/**
 * accel_begin_batch - Begin batch mode
 */
int accel_begin_batch(struct accel_device *dev)
{
    if (!dev)
        return ACCEL_ERR_INVAL;

    dev->batch_mode = true;
    return ACCEL_SUCCESS;
}

/**
 * accel_submit_batch - Submit all batched operations
 */
int accel_submit_batch(struct accel_device *dev)
{
    int ret;

    if (!dev)
        return ACCEL_ERR_INVAL;

    ret = io_uring_submit(&dev->ring);
    dev->batch_mode = false;

    return (ret >= 0) ? ret : ACCEL_ERR_SUBMIT;
}

/**
 * accel_cancel_batch - Cancel pending batch
 */
void accel_cancel_batch(struct accel_device *dev)
{
    if (dev)
        dev->batch_mode = false;
}

/*
 * ===== Memory Mapping =====
 */

/**
 * accel_mmap_doorbells - Map doorbell registers
 */
void *accel_mmap_doorbells(struct accel_device *dev, size_t *size)
{
    void *addr;
    size_t map_size = 8192;  /* Map doorbell page */

    if (!dev)
        return NULL;

    addr = mmap(NULL, map_size, PROT_READ | PROT_WRITE,
                MAP_SHARED, dev->fd, 0x1000);

    if (addr == MAP_FAILED)
        return NULL;

    if (size)
        *size = map_size;

    return addr;
}

/**
 * accel_munmap_doorbells - Unmap doorbell registers
 */
void accel_munmap_doorbells(struct accel_device *dev, void *addr, size_t size)
{
    (void)dev;
    if (addr && size > 0)
        munmap(addr, size);
}
