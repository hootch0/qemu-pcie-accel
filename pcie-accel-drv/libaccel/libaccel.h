/* SPDX-License-Identifier: MIT */
/*
 * PCIe Accelerator Device - User Mode Library Header with io_uring
 *
 * Copyright (C) 2026
 *
 * This library provides a high-performance user-space API for the PCIe
 * Accelerator device using io_uring for async operations.
 *
 * Key features:
 * - io_uring command passthrough for low-latency I/O
 * - Batched submission with single syscall
 * - Both sync and async APIs
 * - Zero-copy completion handling
 *
 * Usage patterns:
 *
 * 1. Simple sync API (easy to use):
 *    struct accel_device *dev = accel_open("/dev/accel0");
 *    accel_loopback(dev, qid, buffer, size, pattern);
 *    accel_close(dev);
 *
 * 2. Async API with io_uring (high performance):
 *    struct accel_device *dev = accel_open("/dev/accel0");
 *    accel_async_loopback(dev, qid, buffer, size, pattern, &token);
 *    // ... do other work ...
 *    accel_wait_completion(dev, &token, &result);
 *    accel_close(dev);
 *
 * 3. Batched async (maximum throughput):
 *    accel_begin_batch(dev);
 *    for (i = 0; i < count; i++)
 *        accel_async_loopback(dev, qid, buffers[i], sizes[i], 0, &tokens[i]);
 *    accel_submit_batch(dev);
 *    // ... wait for completions ...
 */

#ifndef _LIBACCEL_H
#define _LIBACCEL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * ===== Version Information =====
 */
#define LIBACCEL_VERSION_MAJOR  2
#define LIBACCEL_VERSION_MINOR  0
#define LIBACCEL_VERSION_PATCH  0

/*
 * ===== Error Codes =====
 */
#define ACCEL_SUCCESS           0
#define ACCEL_ERR_OPEN         -1
#define ACCEL_ERR_IOCTL        -2
#define ACCEL_ERR_NOMEM        -3
#define ACCEL_ERR_INVAL        -4
#define ACCEL_ERR_TIMEOUT      -5
#define ACCEL_ERR_IO           -6
#define ACCEL_ERR_NODEV        -7
#define ACCEL_ERR_BUSY         -8
#define ACCEL_ERR_URING        -9   /* io_uring setup failed */
#define ACCEL_ERR_SUBMIT       -10  /* Submission failed */
#define ACCEL_ERR_CANCELED     -11  /* Operation canceled */

/*
 * ===== Command Opcodes =====
 */

/* Admin commands */
#define ACCEL_ADM_CMD_IDENTIFY      0x00
#define ACCEL_ADM_CMD_DELETE_SQ     0x01
#define ACCEL_ADM_CMD_CREATE_SQ     0x02
#define ACCEL_ADM_CMD_DELETE_CQ     0x04
#define ACCEL_ADM_CMD_CREATE_CQ     0x05
#define ACCEL_ADM_CMD_SET_FEATURES  0x09
#define ACCEL_ADM_CMD_GET_FEATURES  0x0A
#define ACCEL_ADM_CMD_P2P_SETUP     0x10
#define ACCEL_ADM_CMD_P2P_TEARDOWN  0x11
#define ACCEL_ADM_CMD_P2P_RING_SETUP 0x12
#define ACCEL_ADM_CMD_P2P_RING_TEARDOWN 0x13

/* I/O commands */
#define ACCEL_CMD_LOOPBACK          0x01
#define ACCEL_CMD_P2P_WRITE         0x02
#define ACCEL_CMD_P2P_READ          0x03

/*
 * ===== Status Codes =====
 */
#define ACCEL_SC_SUCCESS            0x00
#define ACCEL_SC_INVALID_OPCODE     0x01
#define ACCEL_SC_INVALID_FIELD      0x02
#define ACCEL_SC_DMA_ERROR          0x30
#define ACCEL_SC_P2P_PEER_NOT_FOUND 0x21

/*
 * ===== io_uring Command Operations =====
 */
#define ACCEL_URING_CMD_SUBMIT        0
#define ACCEL_URING_CMD_CREATE_QUEUE  1
#define ACCEL_URING_CMD_DELETE_QUEUE  2
#define ACCEL_URING_CMD_SETUP_P2P     3
#define ACCEL_URING_CMD_GET_STATS     4
#define ACCEL_URING_CMD_ADMIN         5

/*
 * ===== Data Structures =====
 */

/**
 * struct accel_cmd - Submission queue entry (64 bytes)
 *
 * Command structure for submitting operations to the device.
 * Layout matches the device's command format.
 */
struct accel_cmd {
    uint8_t  opcode;          /* Command opcode */
    uint8_t  flags;           /* [1:0]=DBD type, [2]=PASID, [3]=PRIV */
    uint16_t cid;             /* Command identifier */
    uint32_t rsvd0;           /* Reserved (was nsid) */
    uint32_t rsvd1;           /* Reserved */

    /* Data Block Descriptor (CDW3-6, 16 bytes) */
    union {
        struct { uint64_t prp1; uint64_t prp2; } prpl;
        struct { uint64_t addr; uint32_t length; uint32_t type; } sgl;
        struct { uint64_t addr; uint64_t rsvd; } hva;
    } dbd;

    uint32_t data_xfer_size;  /* Data transfer size in bytes */
    uint64_t rsvd2;           /* Reserved */

    union {
        struct {
            uint32_t length;      /* Transfer length */
            uint32_t rsvd;
            uint64_t peer_addr;   /* Peer device address */
            uint32_t peer_bdf;    /* Peer BDF */
            uint32_t pasid;       /* PASID if enabled */
        } p2p;
        struct {
            uint32_t length;      /* Buffer length */
            uint32_t pattern;     /* Data pattern */
            uint32_t flags;
            uint32_t rsvd[3];
        } loopback;
        struct {
            uint32_t cdw10;
            uint32_t cdw11;
            uint32_t cdw12;
            uint32_t cdw13;
            uint32_t cdw14;
            uint32_t cdw15;
        } admin;
    } dw;
} __attribute__((packed));

/**
 * struct accel_cqe - Completion queue entry (16 bytes)
 *
 * Completion status returned by the device.
 */
struct accel_cqe {
    uint16_t sq_head;         /* SQ head at completion */
    uint16_t cid;             /* Command ID */
    uint32_t status;          /* Status[0]=phase, Status[31:1]=code */
    uint64_t result;          /* Command-specific result (64-bit) */
} __attribute__((packed));

/**
 * struct accel_stats - Device statistics
 */
struct accel_stats {
    uint64_t cmd_submitted;       /* Total commands submitted */
    uint64_t cmd_completed;       /* Total commands completed */
    uint64_t p2p_transfers;       /* Total P2P transfers */
    uint64_t uring_submissions;   /* io_uring submissions */
    uint64_t uring_completions;   /* io_uring completions */
    uint32_t num_queues;          /* Current number of queues */
    uint32_t num_peers;           /* Current number of P2P peers */
};

/**
 * struct accel_identify - Device identification data
 */
struct accel_identify {
    uint32_t version;         /* Device version */
    uint32_t max_queues;      /* Maximum queue pairs */
    uint32_t max_queue_size;  /* Maximum entries per queue */
    uint32_t p2p_max_peers;   /* Maximum P2P peers */
    uint32_t p2p_max_xfers;   /* Maximum concurrent P2P transfers */
    uint32_t pasid_width;     /* PASID width (0 if disabled) */
};

/**
 * struct accel_async_token - Token for tracking async operations
 *
 * Used to track and wait for async command completions.
 */
struct accel_async_token {
    uint64_t user_data;       /* User-provided identifier */
    void    *buffer;          /* Associated data buffer */
    size_t   length;          /* Buffer length */
    int      status;          /* Completion status */
    uint32_t result;          /* Device result */
};

/**
 * struct accel_device - Device handle (opaque)
 */
struct accel_device;

/*
 * ===== API Functions =====
 */

/*
 * ----- Device Management -----
 */

/**
 * accel_open - Open an accelerator device with io_uring support
 * @dev_path: Path to device (e.g., "/dev/accel0")
 *
 * Opens the device and initializes io_uring for async operations.
 *
 * Returns: Device handle on success, NULL on failure
 */
struct accel_device *accel_open(const char *dev_path);

/**
 * accel_open_with_params - Open with custom io_uring parameters
 * @dev_path: Path to device
 * @sq_entries: io_uring submission queue entries (0 for default)
 * @flags: io_uring setup flags (0 for default)
 *
 * Returns: Device handle on success, NULL on failure
 */
struct accel_device *accel_open_with_params(const char *dev_path,
                                            unsigned int sq_entries,
                                            unsigned int flags);

/**
 * accel_close - Close a device handle
 * @dev: Device handle
 *
 * Waits for pending operations and cleans up resources.
 */
void accel_close(struct accel_device *dev);

/**
 * accel_identify - Get device identification
 * @dev: Device handle
 * @id: Output identification structure
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_identify(struct accel_device *dev, struct accel_identify *id);

/**
 * accel_get_stats - Get device statistics
 * @dev: Device handle
 * @stats: Output statistics structure
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_get_stats(struct accel_device *dev, struct accel_stats *stats);

/**
 * accel_strerror - Get error message for error code
 * @err: Error code
 *
 * Returns: Static error message string
 */
const char *accel_strerror(int err);

/*
 * ----- Queue Management -----
 */

/**
 * accel_create_queue - Create an I/O queue pair
 * @dev: Device handle
 * @qid: Queue ID (1 to max_queues)
 * @sq_size: Submission queue size (entries)
 * @cq_size: Completion queue size (entries)
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_create_queue(struct accel_device *dev, uint16_t qid,
                       uint16_t sq_size, uint16_t cq_size);

/**
 * accel_delete_queue - Delete an I/O queue pair
 * @dev: Device handle
 * @qid: Queue ID to delete
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_delete_queue(struct accel_device *dev, uint16_t qid);

/*
 * ----- Synchronous Command Submission -----
 * These functions submit and wait for completion (blocking).
 */

/**
 * accel_submit_cmd - Submit a command and wait for completion
 * @dev: Device handle
 * @qid: Queue ID (0 for admin queue)
 * @cmd: Command to submit
 * @cqe: Output completion entry (may be NULL)
 * @timeout_ms: Timeout in milliseconds (0 for default)
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_submit_cmd(struct accel_device *dev, uint16_t qid,
                     struct accel_cmd *cmd, struct accel_cqe *cqe,
                     uint32_t timeout_ms);

/**
 * accel_loopback - Perform loopback test (synchronous)
 * @dev: Device handle
 * @qid: Queue ID
 * @data: Data buffer (in/out)
 * @length: Data length in bytes
 * @pattern: XOR pattern (0 for no modification)
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_loopback(struct accel_device *dev, uint16_t qid,
                   void *data, uint32_t length, uint32_t pattern);

/**
 * accel_p2p_write - Write data to peer device (synchronous)
 * @dev: Device handle
 * @qid: Queue ID
 * @peer_bdf: Target peer BDF
 * @local_data: Local data buffer (source)
 * @peer_addr: Offset within peer's CMB memory (destination)
 * @length: Transfer length in bytes
 *
 * Writes data from local host memory to the peer device's CMB
 * memory region. The peer_addr parameter is an offset within the peer's
 * CMB, not an absolute address.
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_p2p_write(struct accel_device *dev, uint16_t qid,
                    uint16_t peer_bdf, const void *local_data,
                    uint64_t peer_addr, uint32_t length);

/**
 * accel_p2p_read - Read data from peer device (synchronous)
 * @dev: Device handle
 * @qid: Queue ID
 * @peer_bdf: Source peer BDF
 * @local_data: Local data buffer (destination)
 * @peer_addr: Offset within peer's CMB memory (source)
 * @length: Transfer length in bytes
 *
 * Reads data from the peer device's CMB memory region to local
 * host memory. The peer_addr parameter is an offset within the peer's
 * CMB, not an absolute address.
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_p2p_read(struct accel_device *dev, uint16_t qid,
                   uint16_t peer_bdf, void *local_data,
                   uint64_t peer_addr, uint32_t length);

/*
 * ----- Asynchronous Command Submission -----
 * These functions submit commands and return immediately.
 * Use accel_wait_completion() or accel_poll_completions() to get results.
 */

/**
 * accel_async_submit_cmd - Submit command asynchronously via io_uring
 * @dev: Device handle
 * @qid: Queue ID
 * @cmd: Command to submit
 * @token: Token for tracking completion (receives user_data)
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_async_submit_cmd(struct accel_device *dev, uint16_t qid,
                           struct accel_cmd *cmd,
                           struct accel_async_token *token);

/**
 * accel_async_loopback - Perform loopback test asynchronously
 * @dev: Device handle
 * @qid: Queue ID
 * @data: Data buffer (in/out)
 * @length: Data length in bytes
 * @pattern: XOR pattern
 * @token: Token for tracking completion
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_async_loopback(struct accel_device *dev, uint16_t qid,
                         void *data, uint32_t length, uint32_t pattern,
                         struct accel_async_token *token);

/**
 * accel_async_p2p_write - Async P2P write
 * @dev: Device handle
 * @qid: Queue ID
 * @peer_bdf: Target peer BDF
 * @local_data: Local data buffer
 * @peer_addr: Offset within peer's CMB memory
 * @length: Transfer length
 * @token: Token for tracking
 *
 * Async version of accel_p2p_write. The peer_addr is an offset within
 * the peer device's CMB memory region.
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_async_p2p_write(struct accel_device *dev, uint16_t qid,
                          uint16_t peer_bdf, const void *local_data,
                          uint64_t peer_addr, uint32_t length,
                          struct accel_async_token *token);

/**
 * accel_async_p2p_read - Async P2P read
 * @dev: Device handle
 * @qid: Queue ID
 * @peer_bdf: Source peer BDF
 * @local_data: Local data buffer
 * @peer_addr: Offset within peer's CMB memory
 * @length: Transfer length
 * @token: Token for tracking
 *
 * Async version of accel_p2p_read. The peer_addr is an offset within
 * the peer device's CMB memory region.
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_async_p2p_read(struct accel_device *dev, uint16_t qid,
                         uint16_t peer_bdf, void *local_data,
                         uint64_t peer_addr, uint32_t length,
                         struct accel_async_token *token);

/*
 * ----- Completion Handling -----
 */

/**
 * accel_submit_pending - Submit all pending operations
 * @dev: Device handle
 *
 * Submits any batched operations to io_uring.
 * Call after queueing multiple async operations.
 *
 * Returns: Number of operations submitted, or negative error
 */
int accel_submit_pending(struct accel_device *dev);

/**
 * accel_wait_completion - Wait for a specific completion
 * @dev: Device handle
 * @token: Token from async submit
 * @result: Output result value (may be NULL)
 *
 * Blocks until the operation completes.
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_wait_completion(struct accel_device *dev,
                          struct accel_async_token *token,
                          uint32_t *result);

/**
 * accel_poll_completions - Poll for completions (non-blocking)
 * @dev: Device handle
 * @tokens: Array of tokens to check/fill
 * @max_tokens: Maximum tokens to return
 *
 * Returns completed tokens. Non-blocking.
 *
 * Returns: Number of completions, or negative error
 */
int accel_poll_completions(struct accel_device *dev,
                           struct accel_async_token *tokens,
                           int max_tokens);

/**
 * accel_wait_completions - Wait for multiple completions
 * @dev: Device handle
 * @tokens: Array of tokens to check/fill
 * @max_tokens: Maximum tokens to return
 * @min_complete: Minimum completions to wait for
 * @timeout_ms: Timeout in milliseconds (0 = infinite)
 *
 * Returns: Number of completions, or negative error
 */
int accel_wait_completions(struct accel_device *dev,
                           struct accel_async_token *tokens,
                           int max_tokens, int min_complete,
                           uint32_t timeout_ms);

/**
 * accel_get_pending_count - Get number of pending operations
 * @dev: Device handle
 *
 * Returns: Number of operations pending completion
 */
int accel_get_pending_count(struct accel_device *dev);

/*
 * ----- Batch Operations -----
 * For maximum throughput, batch multiple operations before submitting.
 */

/**
 * accel_begin_batch - Begin a batch of operations
 * @dev: Device handle
 *
 * Operations submitted after this call will be batched until
 * accel_submit_batch() is called.
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_begin_batch(struct accel_device *dev);

/**
 * accel_submit_batch - Submit all batched operations
 * @dev: Device handle
 *
 * Submits all operations batched since accel_begin_batch().
 *
 * Returns: Number submitted, or negative error
 */
int accel_submit_batch(struct accel_device *dev);

/**
 * accel_cancel_batch - Cancel pending batch operations
 * @dev: Device handle
 *
 * Discards any batched operations not yet submitted.
 */
void accel_cancel_batch(struct accel_device *dev);

/*
 * ----- P2P Operations -----
 */

/**
 * accel_setup_p2p_peer - Register a P2P peer device
 * @dev: Device handle
 * @peer_bdf: Peer device BDF (bus << 8 | devfn)
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_setup_p2p_peer(struct accel_device *dev, uint16_t peer_bdf);

/**
 * accel_p2p_ring_setup - Set up P2P ring buffer via admin command
 * @dev: Device handle
 * @peer_bdf: Peer device BDF (bus << 8 | devfn)
 * @slot: Slot for this peer in our device (0-6)
 * @peer_slot: Our slot in the peer's device (0-6)
 *
 * Issues admin command to configure a P2P ring buffer slot on the device.
 * The driver resolves peer BAR0 address from PCI config space.
 * After setup, the devices exchange messages directly via ring buffers
 * in BAR2 CMB without host involvement.
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_p2p_ring_setup(struct accel_device *dev, uint16_t peer_bdf,
                         uint8_t slot, uint8_t peer_slot);

/**
 * accel_p2p_ring_teardown - Tear down a P2P ring buffer
 * @dev: Device handle
 * @slot: Slot number to tear down (0-6)
 *
 * Returns: ACCEL_SUCCESS or error code
 */
int accel_p2p_ring_teardown(struct accel_device *dev, uint8_t slot);

/*
 * ----- Memory Mapping -----
 */

/**
 * accel_mmap_doorbells - Map doorbell registers for direct access
 * @dev: Device handle
 * @size: Output size of mapped region
 *
 * Maps the device's doorbell registers to user space for
 * ultra-low-latency command submission.
 *
 * Returns: Mapped address on success, NULL on failure
 */
void *accel_mmap_doorbells(struct accel_device *dev, size_t *size);

/**
 * accel_munmap_doorbells - Unmap doorbell registers
 * @dev: Device handle
 * @addr: Address returned by accel_mmap_doorbells
 * @size: Size returned by accel_mmap_doorbells
 */
void accel_munmap_doorbells(struct accel_device *dev, void *addr, size_t size);

/*
 * ----- io_uring Direct Access -----
 * For advanced users who want direct io_uring access.
 */

/**
 * accel_get_uring_fd - Get the io_uring file descriptor
 * @dev: Device handle
 *
 * Returns the io_uring fd for direct access. Advanced users can
 * use this with liburing directly.
 *
 * Returns: fd on success, -1 on failure
 */
int accel_get_uring_fd(struct accel_device *dev);

/**
 * accel_get_device_fd - Get the device file descriptor
 * @dev: Device handle
 *
 * Returns: fd on success, -1 on failure
 */
int accel_get_device_fd(struct accel_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* _LIBACCEL_H */
