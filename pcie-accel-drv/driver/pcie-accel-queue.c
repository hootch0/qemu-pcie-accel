// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe Accelerator Device - Queue Management with Async io_uring Support
 *
 * Copyright (C) 2026
 *
 * This file implements queue management for the PCIe Accelerator with
 * full support for io_uring async command submission and completion.
 *
 * Key features:
 * - I/O queue creation/deletion via admin commands
 * - Async command submission with io_uring tracking
 * - Request hash table for O(1) completion lookup
 * - Threaded interrupt handler for io_uring completion delivery
 * - Lock-free completion detection via phase bits
 *
 * io_uring Integration:
 * Commands submitted via io_uring SQEs are tracked through the request
 * lifecycle. On completion, the corresponding io_uring CQE is posted
 * via io_uring_cmd_done() from the threaded IRQ handler.
 */

#include <linux/kernel.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/version.h>
#include <linux/io_uring.h>
#include <linux/io_uring/cmd.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/sched/mm.h>

#include "pcie-accel.h"

/* Workqueue for deferred read completions (copy_to_user needs process context) */
static struct workqueue_struct *accel_completion_wq;

/*
 * Kernel 6.17+ compatibility:
 * - del_timer_sync renamed to timer_delete_sync
 * - from_timer removed, use container_of directly
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define del_timer_sync(t) timer_delete_sync(t)
#endif

/* External function declarations */
extern unsigned int accel_get_req_pool_size(void);

/* Default I/O queue parameters */
#define DEFAULT_IO_QUEUE_SIZE	256
#define MAX_IO_QUEUE_SIZE	4096

/* Global command ID counter - shared across all queues to avoid collision */
static atomic_t global_cid_counter = ATOMIC_INIT(0);

/**
 * accel_read_completion_work - Workqueue handler for read completions
 * @work: Work structure embedded in request
 *
 * Runs in process context to safely call copy_to_user for read operations.
 * Threaded IRQ context cannot access user memory directly.
 */
static void accel_read_completion_work(struct work_struct *work)
{
	struct accel_request *req = container_of(work, struct accel_request,
						 completion_work);
	struct accel_dev *dev = req->queue->dev;
	int err = 0;

	/* Copy data to user space using the saved mm context */
	if (req->user_buf && req->data_buf && req->data_len > 0 && req->mm) {
		kthread_use_mm(req->mm);
		if (copy_to_user(req->user_buf, req->data_buf, req->data_len)) {
			dev_err(&dev->pdev->dev,
				"copy_to_user failed in workqueue\n");
			err = -EFAULT;
		}
		kthread_unuse_mm(req->mm);
	}

	/* Complete the io_uring command */
	io_uring_cmd_done(req->ioucmd, err, req->result, IO_URING_F_UNLOCKED);
	atomic64_inc(&dev->uring_completions);

	/* Free the request */
	accel_free_request(req);
}

/**
 * accel_queue_init - Initialize queue subsystem
 *
 * Creates the completion workqueue. Called during module init.
 *
 * Returns: 0 on success, negative error on failure
 */
int accel_queue_init(void)
{
	accel_completion_wq = alloc_workqueue("pcie-accel-cpl",
					      WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!accel_completion_wq)
		return -ENOMEM;

	return 0;
}

/**
 * accel_queue_exit - Cleanup queue subsystem
 *
 * Destroys the completion workqueue. Called during module exit.
 */
void accel_queue_exit(void)
{
	if (accel_completion_wq) {
		flush_workqueue(accel_completion_wq);
		destroy_workqueue(accel_completion_wq);
		accel_completion_wq = NULL;
	}
}

/*
 * ===== Doorbell Operations =====
 *
 * Doorbells are memory-mapped registers that notify the device of new
 * submissions or completed processing. They follow NVMe conventions:
 * - SQ doorbell: Write new tail pointer to notify of new commands
 * - CQ doorbell: Write new head pointer to acknowledge processed completions
 *
 * Doorbell Register Layout (starting at offset 0x1000):
 *   0x1000 + (2 * qid * stride) + 0: SQ tail doorbell
 *   0x1000 + (2 * qid * stride) + 4: CQ head doorbell
 * Where stride is typically 4 bytes (configured in CAP register).
 */

/**
 * accel_ring_sq_doorbell - Ring submission queue doorbell
 * @queue: Queue structure
 *
 * Writes the new SQ tail to the doorbell register to notify the device
 * that new commands are available for processing.
 *
 * The doorbell write triggers the device to read new SQEs starting from
 * the previous tail to the new tail value.
 *
 * Returns: 0 on success
 */
int accel_ring_sq_doorbell(struct accel_queue *queue)
{
	struct accel_dev *dev = queue->dev;
	u32 db_offset;

	/*
	 * SQ doorbell offset calculation:
	 * Base offset: 0x1000 (ACCEL_REG_DOORBELL)
	 * SQ doorbell: 0x1000 + (2 * qid * 4) = 0x1000 + (8 * qid)
	 *
	 * We use stride of 4 bytes between doorbell registers.
	 */
	db_offset = ACCEL_REG_DOORBELL + (queue->qid * 8);

	/* Write new tail value - this triggers device to process new commands */
	writel(queue->sq_tail, dev->bar0 + db_offset);

	return 0;
}

/**
 * accel_ring_cq_doorbell - Ring completion queue doorbell
 * @queue: Queue structure
 *
 * Writes the new CQ head to the doorbell register to acknowledge
 * that completions have been processed by the host.
 *
 * This allows the device to reuse CQ slots for future completions.
 *
 * Returns: 0 on success
 */
int accel_ring_cq_doorbell(struct accel_queue *queue)
{
	struct accel_dev *dev = queue->dev;
	u32 db_offset;

	/*
	 * CQ doorbell offset calculation:
	 * CQ doorbell: 0x1000 + (2 * qid + 1) * 4 = 0x1000 + (8 * qid) + 4
	 */
	db_offset = ACCEL_REG_DOORBELL + (queue->qid * 8) + 4;

	/* Write new head value - acknowledges processed completions */
	writel(queue->cq_head, dev->bar0 + db_offset);

	return 0;
}

/*
 * ===== Request Management =====
 *
 * For io_uring async operations, we need to track in-flight requests
 * to match completions back to their originating io_uring commands.
 *
 * Request lifecycle:
 * 1. Allocate from slab cache
 * 2. Add to hash table keyed by CID
 * 3. Add to pending list
 * 4. Submit to device
 * 5. On completion IRQ: lookup by CID, call io_uring_cmd_done()
 * 6. Remove from hash/pending, return to cache
 */

/**
 * accel_alloc_request - Allocate a request tracking structure
 * @queue: Queue structure
 *
 * Allocates a request from the device's slab cache for tracking
 * an async command through io_uring.
 *
 * Returns: Request structure, or NULL on allocation failure
 */
struct accel_request *accel_alloc_request(struct accel_queue *queue)
{
	struct accel_dev *dev = queue->dev;
	struct accel_request *req;

	req = kmem_cache_alloc(dev->req_cache, GFP_ATOMIC);
	if (!req)
		return NULL;

	memset(req, 0, sizeof(*req));
	req->queue = queue;
	INIT_HLIST_NODE(&req->hash_node);
	INIT_LIST_HEAD(&req->list);

	return req;
}

/**
 * __accel_free_request - Internal request free with timer control
 * @req: Request to free
 * @sync_timer: If true, wait for timer to complete; if false, just deactivate
 *
 * Returns the request to the slab cache. Must be called after
 * the request is removed from all tracking structures.
 */
static void __accel_free_request(struct accel_request *req, bool sync_timer)
{
	struct accel_dev *dev = req->queue->dev;

	/* Free any associated DMA buffer */
	if (req->data_buf && req->data_len > 0) {
		dma_free_coherent(&dev->pdev->dev, req->data_len,
				  req->data_buf, req->data_dma);
	}

	/* Release mm reference if held */
	if (req->mm) {
		mmput(req->mm);
		req->mm = NULL;
	}

	/*
	 * Cancel timeout timer. Use del_timer_sync when called from normal
	 * context, but just del_timer when called from the timer callback
	 * itself (to avoid deadlock).
	 */
	if (sync_timer)
		del_timer_sync(&req->timer);
	else
		del_timer(&req->timer);

	kmem_cache_free(dev->req_cache, req);
}

/**
 * accel_free_request - Free a request tracking structure
 * @req: Request to free
 *
 * Returns the request to the slab cache. Must be called after
 * the request is removed from all tracking structures.
 */
void accel_free_request(struct accel_request *req)
{
	__accel_free_request(req, true);
}

/**
 * accel_find_request - Find request by command ID
 * @queue: Queue structure
 * @cid: Command ID to find
 *
 * Looks up a request in the hash table by its command ID.
 * Must be called with appropriate locking.
 *
 * Returns: Request structure, or NULL if not found
 */
static struct accel_request *accel_find_request(struct accel_queue *queue, u16 cid)
{
	struct accel_request *req;

	hash_for_each_possible(queue->req_hash, req, hash_node, cid) {
		if (req->cid == cid)
			return req;
	}

	return NULL;
}

/**
 * accel_request_timeout - Timeout callback for requests
 * @t: Timer structure
 *
 * Called when a request times out. Completes the io_uring command
 * with an error status.
 */
static void accel_request_timeout(struct timer_list *t)
{
	struct accel_request *req = container_of(t, struct accel_request, timer);
	struct accel_queue *queue = req->queue;
	unsigned long flags;

	spin_lock_irqsave(&queue->cq_lock, flags);

	/* Check if request is still pending */
	if (!hlist_unhashed(&req->hash_node)) {
		hash_del(&req->hash_node);
		list_del(&req->list);
		spin_unlock_irqrestore(&queue->cq_lock, flags);

		/* Complete io_uring command with timeout error */
		if (req->ioucmd)
			io_uring_cmd_done(req->ioucmd, -ETIMEDOUT, 0,
					  IO_URING_F_UNLOCKED);

		/* Use non-syncing free - we're in the timer callback */
		__accel_free_request(req, false);
	} else {
		spin_unlock_irqrestore(&queue->cq_lock, flags);
	}
}

/*
 * ===== Completion Processing =====
 *
 * Completion Queue Entry (CQE) Format (16 bytes):
 *   Offset 0x00: result (4 bytes) - Command-specific result
 *   Offset 0x04: reserved (4 bytes)
 *   Offset 0x08: sq_head (2 bytes) - SQ head at completion time
 *   Offset 0x0A: sq_id (2 bytes) - Originating SQ ID
 *   Offset 0x0C: cid (2 bytes) - Command ID
 *   Offset 0x0E: status (2 bytes) - Status with phase bit in bit 0
 *
 * Phase Bit Protocol:
 * The phase bit (bit 0 of status) alternates between 0 and 1 each time
 * the CQ wraps around. This allows lock-free detection of new completions
 * without requiring explicit signaling from the device.
 */

/**
 * accel_cqe_valid - Check if a CQE is valid (has the expected phase bit)
 * @cqe: Completion queue entry
 * @phase: Expected phase bit
 *
 * Returns: true if CQE is valid, false otherwise
 */
static inline bool accel_cqe_valid(struct accel_cqe *cqe, u8 phase)
{
	/*
	 * Read the status word and check phase bit.
	 * The device sets the phase bit to match our expected value
	 * when writing a new completion.
	 */
	return (le16_to_cpu(cqe->status) & 0x1) == phase;
}

/**
 * accel_process_cq - Process completion queue entries (hardirq context)
 * @queue: Queue structure
 *
 * Processes all pending CQEs and wakes up threaded handler.
 * Called from hardirq context - must be fast and non-blocking.
 *
 * Returns: Number of completions detected
 */
static int accel_process_cq_hardirq(struct accel_queue *queue)
{
	struct accel_cqe *cqe;
	int detected = 0;

	/* Quick check for any completions */
	cqe = queue->cq_buffer + (queue->cq_head * ACCEL_CQE_SIZE);
	if (accel_cqe_valid(cqe, queue->cq_phase))
		detected = 1;

	return detected;
}

/**
 * accel_process_cq_threaded - Process completions in threaded context
 * @queue: Queue structure
 *
 * Processes all pending CQEs and delivers io_uring completions.
 * Called from threaded IRQ context - can sleep and do complex work.
 *
 * Returns: Number of completions processed
 */
static int accel_process_cq_threaded(struct accel_queue *queue)
{
	struct accel_dev *dev = queue->dev;
	struct accel_cqe *cqe;
	struct accel_request *req;
	unsigned long flags;
	int processed = 0;
	u16 cid;
	s32 result;
	u16 status;

	spin_lock_irqsave(&queue->cq_lock, flags);

	while (1) {
		cqe = queue->cq_buffer + (queue->cq_head * ACCEL_CQE_SIZE);

		/* Check if this CQE is valid using phase bit */
		if (!accel_cqe_valid(cqe, queue->cq_phase))
			break;

		/*
		 * Read memory barrier to ensure we see the full CQE
		 * after confirming the phase bit is valid.
		 */
		rmb();

		/* Extract completion data */
		cid = le16_to_cpu(cqe->cid);
		result = le32_to_cpu(cqe->result);
		status = le16_to_cpu(cqe->status) >> 1;  /* Remove phase bit */

		/* Update SQ head from completion */
		queue->sq_head = le16_to_cpu(cqe->sq_head);

		/* Find the request by CID */
		req = accel_find_request(queue, cid);
		if (req) {
			/* Remove from tracking structures */
			hash_del(&req->hash_node);
			list_del(&req->list);

			/* Store completion data in request */
			memcpy(&req->cqe, cqe, sizeof(*cqe));
			req->status = status;

			spin_unlock_irqrestore(&queue->cq_lock, flags);

			/* Complete the io_uring command */
			if (req->ioucmd) {
				/*
				 * Convert device status to errno.
				 * Status 0 = success, others are errors.
				 */
				int err = (status == 0) ? 0 : -EIO;

				if (status != 0) {
					dev_dbg(&dev->pdev->dev,
						"Command cid=%u opcode=%u failed: status=0x%x\n",
						cid, req->cmd.opcode, status);
				}

				/*
				 * For read operations, defer completion to
				 * workqueue so copy_to_user runs in process
				 * context. Threaded IRQ context cannot safely
				 * access user memory.
				 */
				if (err == 0 && req->is_read && req->user_buf &&
				    req->data_buf && req->data_len > 0) {
					req->result = result;
					INIT_WORK(&req->completion_work,
						  accel_read_completion_work);
					queue_work(accel_completion_wq,
						   &req->completion_work);
					/* Request freed by work handler */
					spin_lock_irqsave(&queue->cq_lock,
							  flags);
					continue;
				}

				io_uring_cmd_done(req->ioucmd, err, result,
						  IO_URING_F_UNLOCKED);
				atomic64_inc(&dev->uring_completions);
			}

			accel_free_request(req);

			spin_lock_irqsave(&queue->cq_lock, flags);
		}

		processed++;

		/* Advance CQ head with wrap-around and phase flip */
		queue->cq_head++;
		if (queue->cq_head >= queue->cq_size) {
			queue->cq_head = 0;
			queue->cq_phase = !queue->cq_phase;
		}
	}

	spin_unlock_irqrestore(&queue->cq_lock, flags);

	if (processed > 0) {
		/* Acknowledge completions by writing CQ doorbell */
		accel_ring_cq_doorbell(queue);

		/* Update statistics */
		atomic64_add(processed, &dev->cmd_completed);
	}

	return processed;
}

/**
 * accel_irq_handler - MSI-X hardirq handler
 * @irq: Interrupt number
 * @data: Queue pointer
 *
 * Called in hardirq context when the device fires an interrupt.
 * Does minimal processing and returns IRQ_WAKE_THREAD to schedule
 * the threaded handler for io_uring completion delivery.
 *
 * Returns: IRQ_WAKE_THREAD if work to do, IRQ_NONE otherwise
 */
irqreturn_t accel_irq_handler(int irq, void *data)
{
	struct accel_queue *queue = data;

	/* Check if there are any completions to process */
	if (accel_process_cq_hardirq(queue) > 0)
		return IRQ_WAKE_THREAD;

	return IRQ_NONE;
}

/**
 * accel_irq_handler_threaded - Threaded IRQ handler
 * @irq: Interrupt number
 * @data: Queue pointer
 *
 * Called in threaded context to process completions and deliver
 * io_uring CQEs. Can sleep and perform complex operations.
 *
 * Returns: IRQ_HANDLED
 */
irqreturn_t accel_irq_handler_threaded(int irq, void *data)
{
	struct accel_queue *queue = data;

	/* Process all pending completions */
	accel_process_cq_threaded(queue);

	return IRQ_HANDLED;
}

/*
 * ===== Command Submission =====
 *
 * Commands are submitted to Submission Queues (SQs) and completions
 * are received from Completion Queues (CQs). Each queue pair shares
 * a command ID (CID) namespace for matching.
 */

/**
 * accel_submit_cmd - Submit a command to a submission queue
 * @queue: Queue structure
 * @cmd: Command to submit
 *
 * Copies the command to the SQ and advances the tail pointer.
 * Does NOT ring the doorbell - caller must do that.
 *
 * Returns: 0 on success, -ENOSPC if queue is full
 */
static int accel_submit_cmd(struct accel_queue *queue, struct accel_cmd *cmd)
{
	u32 next_tail;
	void *sqe;

	/*
	 * Check if queue is full.
	 * Queue is full when next_tail would equal head (all slots used).
	 */
	next_tail = (queue->sq_tail + 1) % queue->sq_size;
	if (next_tail == queue->sq_head)
		return -ENOSPC;

	/* Copy command to SQ slot */
	sqe = queue->sq_buffer + (queue->sq_tail * ACCEL_SQE_SIZE);
	memcpy(sqe, cmd, sizeof(*cmd));

	/*
	 * Write memory barrier to ensure command is fully written
	 * before we advance the tail pointer.
	 */
	wmb();

	queue->sq_tail = next_tail;

	return 0;
}

/**
 * accel_submit_async_cmd - Submit command for async io_uring completion
 * @queue: Queue structure
 * @cmd: Command to submit
 * @ioucmd: io_uring command context for completion
 * @data_buf: Optional DMA buffer for data transfer
 * @data_dma: DMA address of data buffer
 * @data_len: Length of data buffer
 * @user_buf: User buffer address for read operations (copy back on completion)
 *
 * Submits a command and tracks it for async completion via io_uring.
 * The completion will be delivered through io_uring_cmd_done().
 *
 * Returns: 0 on success, negative error on failure
 */
int accel_submit_async_cmd(struct accel_queue *queue, struct accel_cmd *cmd,
			   struct io_uring_cmd *ioucmd, void *data_buf,
			   dma_addr_t data_dma, size_t data_len,
			   void __user *user_buf)
{
	struct accel_dev *dev = queue->dev;
	struct accel_request *req;
	unsigned long flags;
	u16 cid;
	int ret;
	u8 opcode = cmd->opcode;

	/* Allocate request tracking structure */
	req = accel_alloc_request(queue);
	if (!req)
		return -ENOMEM;

	/* Assign unique command ID (wrap at 16 bits) */
	cid = atomic_inc_return(&global_cid_counter) & 0xFFFF;
	cmd->cid = cpu_to_le16(cid);

	/* Initialize request */
	req->cid = cid;
	req->ioucmd = ioucmd;
	req->data_buf = data_buf;
	req->data_dma = data_dma;
	req->data_len = data_len;
	req->user_buf = user_buf;
	req->is_read = (opcode == ACCEL_CMD_P2P_READ ||
			opcode == ACCEL_CMD_CXL_READ);
	req->mm = NULL;
	if (req->is_read && req->user_buf) {
		req->mm = current->mm;
		mmget(req->mm);
	}
	req->start_time = jiffies;
	memcpy(&req->cmd, cmd, sizeof(*cmd));

	/* Set up timeout timer */
	timer_setup(&req->timer, accel_request_timeout, 0);
	mod_timer(&req->timer, jiffies + msecs_to_jiffies(5000));

	spin_lock_irqsave(&queue->sq_lock, flags);

	/* Submit command to device */
	ret = accel_submit_cmd(queue, cmd);
	if (ret) {
		spin_unlock_irqrestore(&queue->sq_lock, flags);
		del_timer_sync(&req->timer);
		if (req->mm)
			mmput(req->mm);
		kmem_cache_free(dev->req_cache, req);
		return ret;
	}

	/* Add to tracking structures */
	spin_lock(&queue->cq_lock);
	hash_add(queue->req_hash, &req->hash_node, cid);
	list_add_tail(&req->list, &queue->req_pending);
	spin_unlock(&queue->cq_lock);

	/* Ring doorbell to notify device */
	accel_ring_sq_doorbell(queue);

	spin_unlock_irqrestore(&queue->sq_lock, flags);

	/* Update statistics */
	atomic64_inc(&dev->cmd_submitted);
	atomic64_inc(&dev->uring_submissions);
	atomic64_inc(&queue->submitted);

	return 0;
}

/**
 * accel_complete_request - Manually complete a request
 * @req: Request to complete
 *
 * Called to complete a request that doesn't go through the normal
 * completion path (e.g., error handling).
 */
void accel_complete_request(struct accel_request *req)
{
	struct accel_queue *queue = req->queue;
	unsigned long flags;

	spin_lock_irqsave(&queue->cq_lock, flags);

	/* Remove from tracking if still tracked */
	if (!hlist_unhashed(&req->hash_node)) {
		hash_del(&req->hash_node);
		list_del(&req->list);
	}

	spin_unlock_irqrestore(&queue->cq_lock, flags);

	accel_free_request(req);
}

/*
 * ===== Synchronous Command Submission =====
 *
 * For admin commands and compatibility, we also support synchronous
 * submission with polling/wait for completion.
 */

/**
 * accel_wait_for_completion - Wait for a specific command to complete
 * @queue: Queue structure
 * @cid: Command ID to wait for
 * @cqe: Output CQE buffer
 * @timeout_ms: Timeout in milliseconds
 *
 * Polls/waits for the command with the given CID to complete.
 * Used for synchronous admin commands.
 *
 * Returns: 0 on success, -ETIMEDOUT on timeout, -EINTR if interrupted
 */
static int accel_wait_for_completion(struct accel_queue *queue, u16 cid,
				     struct accel_cqe *cqe, u32 timeout_ms)
{
	struct accel_cqe *q_cqe;
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	unsigned long flags;

	while (time_before(jiffies, deadline)) {
		/* Process any pending completions */
		accel_process_cq_threaded(queue);

		spin_lock_irqsave(&queue->cq_lock, flags);

		/* Search for our CID in processed completions */
		for (u32 i = 0; i < queue->cq_size; i++) {
			q_cqe = queue->cq_buffer + (i * ACCEL_CQE_SIZE);
			if (le16_to_cpu(q_cqe->cid) == cid &&
			    accel_cqe_valid(q_cqe, queue->cq_phase)) {
				if (cqe)
					memcpy(cqe, q_cqe, sizeof(*cqe));
				spin_unlock_irqrestore(&queue->cq_lock, flags);
				return 0;
			}
		}

		spin_unlock_irqrestore(&queue->cq_lock, flags);

		/* Short sleep before retrying */
		usleep_range(100, 200);
	}

	return -ETIMEDOUT;
}

/**
 * accel_submit_admin_cmd - Submit and wait for an admin command
 * @dev: Device structure
 * @cmd: Command to submit
 * @cqe: Output CQE buffer (optional)
 *
 * Submits a command to the admin queue and waits for completion.
 * Used for device management operations like queue creation.
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_submit_admin_cmd(struct accel_dev *dev, struct accel_cmd *cmd,
			   struct accel_cqe *cqe)
{
	struct accel_queue *queue = dev->queues[0];
	unsigned long flags;
	u16 cid;
	int ret;

	if (!queue)
		return -ENODEV;

	/* Assign unique command ID */
	cid = atomic_inc_return(&global_cid_counter) & 0xFFFF;
	cmd->cid = cpu_to_le16(cid);

	spin_lock_irqsave(&queue->sq_lock, flags);

	/* Submit command */
	ret = accel_submit_cmd(queue, cmd);
	if (ret) {
		spin_unlock_irqrestore(&queue->sq_lock, flags);
		return ret;
	}

	/* Ring doorbell */
	accel_ring_sq_doorbell(queue);

	spin_unlock_irqrestore(&queue->sq_lock, flags);

	/* Update statistics */
	atomic64_inc(&dev->cmd_submitted);

	/* Wait for completion */
	ret = accel_wait_for_completion(queue, cid, cqe, 5000);
	if (ret == 0)
		atomic64_inc(&dev->cmd_completed);

	return ret;
}

/**
 * accel_submit_sync_cmd - Submit and wait for an I/O command
 * @dev: Device structure
 * @qid: Queue ID
 * @cmd: Command to submit
 * @cqe: Output CQE buffer
 * @timeout_ms: Timeout in milliseconds
 *
 * Synchronous I/O command submission for compatibility with
 * non-io_uring paths.
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_submit_sync_cmd(struct accel_dev *dev, u16 qid,
			  struct accel_cmd *cmd, struct accel_cqe *cqe,
			  u32 timeout_ms)
{
	struct accel_queue *queue;
	unsigned long flags;
	u16 cid;
	int ret;

	if (qid >= ACCEL_MAX_QUEUES)
		return -EINVAL;

	queue = dev->queues[qid];
	if (!queue)
		return -ENODEV;

	/* Assign unique command ID */
	cid = atomic_inc_return(&global_cid_counter) & 0xFFFF;
	cmd->cid = cpu_to_le16(cid);

	spin_lock_irqsave(&queue->sq_lock, flags);

	ret = accel_submit_cmd(queue, cmd);
	if (ret) {
		spin_unlock_irqrestore(&queue->sq_lock, flags);
		return ret;
	}

	accel_ring_sq_doorbell(queue);

	spin_unlock_irqrestore(&queue->sq_lock, flags);

	atomic64_inc(&dev->cmd_submitted);

	ret = accel_wait_for_completion(queue, cid, cqe, timeout_ms);
	if (ret == 0)
		atomic64_inc(&dev->cmd_completed);

	return ret;
}

/*
 * ===== I/O Queue Management =====
 *
 * I/O queues are created via admin commands after the controller is
 * enabled. Each queue pair (SQ + CQ) can process commands independently.
 */

/**
 * accel_init_request_pool - Initialize per-queue request pool
 * @queue: Queue structure
 *
 * Pre-allocates a pool of request structures for the queue to
 * reduce allocation overhead during hot-path operations.
 *
 * Returns: 0 on success, negative error on failure
 */
static int accel_init_request_pool(struct accel_queue *queue)
{
	struct accel_dev *dev = queue->dev;
	u32 pool_size = accel_get_req_pool_size();
	u32 i;

	queue->req_pool_size = min(pool_size, queue->sq_size);

	queue->req_pool = kcalloc(queue->req_pool_size,
				  sizeof(struct accel_request),
				  GFP_KERNEL);
	if (!queue->req_pool)
		return -ENOMEM;

	/* Initialize all requests and add to free list */
	for (i = 0; i < queue->req_pool_size; i++) {
		struct accel_request *req = &queue->req_pool[i];
		req->queue = queue;
		INIT_HLIST_NODE(&req->hash_node);
		INIT_LIST_HEAD(&req->list);
		list_add_tail(&req->list, &queue->req_free);
	}

	dev_dbg(&dev->pdev->dev, "Queue %u: initialized %u request pool\n",
		queue->qid, queue->req_pool_size);

	return 0;
}

/**
 * accel_create_queue - Create an I/O queue pair
 * @dev: Device structure
 * @qid: Queue ID (1 to max_queues)
 * @sq_size: Submission queue size
 * @cq_size: Completion queue size
 *
 * Allocates DMA memory and sends admin commands to create the queue pair.
 *
 * Create CQ Admin Command (opcode 0x05):
 *   CDW10[15:0]: CQID - Completion Queue Identifier
 *   CDW10[31:16]: QSIZE - Queue Size (0-based)
 *   CDW11[15:0]: IV - Interrupt Vector
 *   CDW11[16]: IEN - Interrupts Enabled
 *   PRP1: CQ base address (page-aligned)
 *
 * Create SQ Admin Command (opcode 0x01):
 *   CDW10[15:0]: SQID - Submission Queue Identifier
 *   CDW10[31:16]: QSIZE - Queue Size (0-based)
 *   CDW11[15:0]: CQID - Associated Completion Queue ID
 *   PRP1: SQ base address (page-aligned)
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_create_queue(struct accel_dev *dev, u16 qid, u16 sq_size, u16 cq_size)
{
	struct accel_queue *queue;
	struct accel_cmd cmd;
	struct accel_cqe cqe;
	int ret;

	if (qid == 0 || qid >= ACCEL_MAX_QUEUES)
		return -EINVAL;

	if (dev->queues[qid])
		return -EEXIST;

	/* Clamp queue sizes to valid range */
	if (sq_size < 2 || sq_size > MAX_IO_QUEUE_SIZE)
		sq_size = DEFAULT_IO_QUEUE_SIZE;
	if (cq_size < 2 || cq_size > MAX_IO_QUEUE_SIZE)
		cq_size = DEFAULT_IO_QUEUE_SIZE;

	queue = kzalloc(sizeof(*queue), GFP_KERNEL);
	if (!queue)
		return -ENOMEM;

	queue->dev = dev;
	queue->qid = qid;
	queue->sq_size = sq_size;
	queue->cq_size = cq_size;
	queue->cq_phase = 1;  /* Initial phase bit */

	spin_lock_init(&queue->sq_lock);
	spin_lock_init(&queue->cq_lock);

	/* Initialize request tracking */
	hash_init(queue->req_hash);
	INIT_LIST_HEAD(&queue->req_free);
	INIT_LIST_HEAD(&queue->req_pending);
	atomic64_set(&queue->submitted, 0);
	atomic64_set(&queue->completed, 0);

	/* Allocate SQ buffer (page-aligned for DMA) */
	queue->sq_buffer = dma_alloc_coherent(&dev->pdev->dev,
					      sq_size * ACCEL_SQE_SIZE,
					      &queue->sq_dma_addr,
					      GFP_KERNEL);
	if (!queue->sq_buffer) {
		ret = -ENOMEM;
		goto err_free_queue;
	}

	/* Allocate CQ buffer (page-aligned for DMA) */
	queue->cq_buffer = dma_alloc_coherent(&dev->pdev->dev,
					      cq_size * ACCEL_CQE_SIZE,
					      &queue->cq_dma_addr,
					      GFP_KERNEL);
	if (!queue->cq_buffer) {
		ret = -ENOMEM;
		goto err_free_sq;
	}

	/* Clear queue buffers for proper phase bit detection */
	memset(queue->sq_buffer, 0, sq_size * ACCEL_SQE_SIZE);
	memset(queue->cq_buffer, 0, cq_size * ACCEL_CQE_SIZE);

	/*
	 * Create Completion Queue via admin command.
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = ACCEL_ADM_CMD_CREATE_CQ;
	cmd.prp1 = cpu_to_le64(queue->cq_dma_addr);
	cmd.dw.admin.cdw10 = cpu_to_le32(qid | ((cq_size - 1) << 16));

	/* Assign interrupt vector if available */
	if (qid < dev->num_vecs) {
		queue->irq_vector = dev->msix_entries[qid].vector;
		cmd.dw.admin.cdw11 = cpu_to_le32(qid | (1 << 16));  /* IEN=1 */
	} else {
		cmd.dw.admin.cdw11 = cpu_to_le32(0);  /* No interrupt */
	}

	ret = accel_submit_admin_cmd(dev, &cmd, &cqe);
	if (ret) {
		dev_err(&dev->pdev->dev, "Failed to create CQ %u: %d\n", qid, ret);
		goto err_free_cq;
	}

	/* Check completion status (status is in bits [15:1]) */
	if ((le16_to_cpu(cqe.status) >> 1) != 0) {
		dev_err(&dev->pdev->dev, "Create CQ %u failed: status 0x%x\n",
			qid, le16_to_cpu(cqe.status) >> 1);
		ret = -EIO;
		goto err_free_cq;
	}

	/*
	 * Create Submission Queue via admin command.
	 */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = ACCEL_ADM_CMD_CREATE_SQ;
	cmd.prp1 = cpu_to_le64(queue->sq_dma_addr);
	cmd.dw.admin.cdw10 = cpu_to_le32(qid | ((sq_size - 1) << 16));
	cmd.dw.admin.cdw11 = cpu_to_le32(qid);  /* CQID = SQID */

	ret = accel_submit_admin_cmd(dev, &cmd, &cqe);
	if (ret) {
		dev_err(&dev->pdev->dev, "Failed to create SQ %u: %d\n", qid, ret);
		goto err_delete_cq;
	}

	/* Check completion status */
	if ((le16_to_cpu(cqe.status) >> 1) != 0) {
		dev_err(&dev->pdev->dev, "Create SQ %u failed: status 0x%x\n",
			qid, le16_to_cpu(cqe.status) >> 1);
		ret = -EIO;
		goto err_delete_cq;
	}

	/* Request threaded interrupt handler for io_uring completion */
	if (queue->irq_vector) {
		char irq_name[32];
		snprintf(irq_name, sizeof(irq_name), "pcie-accel-q%u", qid);
		ret = request_threaded_irq(queue->irq_vector,
					   accel_irq_handler,
					   accel_irq_handler_threaded,
					   0, irq_name, queue);
		if (ret) {
			dev_warn(&dev->pdev->dev,
				 "Failed to request IRQ for queue %u\n", qid);
			queue->irq_vector = 0;
		}
	}

	/* Initialize request pool for efficient allocation */
	ret = accel_init_request_pool(queue);
	if (ret) {
		dev_warn(&dev->pdev->dev,
			 "Failed to init request pool for queue %u\n", qid);
		/* Not fatal - can allocate on demand */
	}

	dev->queues[qid] = queue;
	dev->num_queues++;

	dev_dbg(&dev->pdev->dev,
		"Created queue %u: SQ@0x%llx CQ@0x%llx sizes %u/%u\n",
		qid, queue->sq_dma_addr, queue->cq_dma_addr,
		sq_size, cq_size);

	return 0;

err_delete_cq:
	/* Delete the CQ we created */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = ACCEL_ADM_CMD_DELETE_CQ;
	cmd.dw.admin.cdw10 = cpu_to_le32(qid);
	accel_submit_admin_cmd(dev, &cmd, NULL);

err_free_cq:
	dma_free_coherent(&dev->pdev->dev, cq_size * ACCEL_CQE_SIZE,
			  queue->cq_buffer, queue->cq_dma_addr);
err_free_sq:
	dma_free_coherent(&dev->pdev->dev, sq_size * ACCEL_SQE_SIZE,
			  queue->sq_buffer, queue->sq_dma_addr);
err_free_queue:
	kfree(queue);
	return ret;
}

/**
 * accel_delete_queue - Delete an I/O queue pair
 * @dev: Device structure
 * @qid: Queue ID to delete
 *
 * Sends admin commands to delete the queue pair and frees resources.
 *
 * Delete SQ Admin Command (opcode 0x00):
 *   CDW10[15:0]: SQID - Submission Queue Identifier
 *
 * Delete CQ Admin Command (opcode 0x04):
 *   CDW10[15:0]: CQID - Completion Queue Identifier
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_delete_queue(struct accel_dev *dev, u16 qid)
{
	struct accel_queue *queue;
	struct accel_cmd cmd;
	struct accel_request *req;
	struct hlist_node *tmp;
	unsigned long flags;
	int i;
	int ret;

	if (qid == 0 || qid >= ACCEL_MAX_QUEUES)
		return -EINVAL;

	queue = dev->queues[qid];
	if (!queue)
		return -ENOENT;

	/* Cancel all pending requests */
	spin_lock_irqsave(&queue->cq_lock, flags);
	hash_for_each_safe(queue->req_hash, i, tmp, req, hash_node) {
		hash_del(&req->hash_node);
		list_del(&req->list);

		/* Complete with error */
		if (req->ioucmd) {
			spin_unlock_irqrestore(&queue->cq_lock, flags);
			io_uring_cmd_done(req->ioucmd, -ECANCELED, 0,
					  IO_URING_F_UNLOCKED);
			spin_lock_irqsave(&queue->cq_lock, flags);
		}

		/* Don't free pool requests individually */
		if (req < queue->req_pool ||
		    req >= queue->req_pool + queue->req_pool_size) {
			accel_free_request(req);
		}
	}
	spin_unlock_irqrestore(&queue->cq_lock, flags);

	/* Free interrupt handler */
	if (queue->irq_vector)
		free_irq(queue->irq_vector, queue);

	/* Delete Submission Queue via admin command */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = ACCEL_ADM_CMD_DELETE_SQ;
	cmd.dw.admin.cdw10 = cpu_to_le32(qid);
	ret = accel_submit_admin_cmd(dev, &cmd, NULL);
	if (ret)
		dev_warn(&dev->pdev->dev, "Delete SQ %u failed: %d\n", qid, ret);

	/* Delete Completion Queue via admin command */
	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = ACCEL_ADM_CMD_DELETE_CQ;
	cmd.dw.admin.cdw10 = cpu_to_le32(qid);
	ret = accel_submit_admin_cmd(dev, &cmd, NULL);
	if (ret)
		dev_warn(&dev->pdev->dev, "Delete CQ %u failed: %d\n", qid, ret);

	/* Free request pool */
	kfree(queue->req_pool);

	/* Free DMA buffers */
	dma_free_coherent(&dev->pdev->dev,
			  queue->sq_size * ACCEL_SQE_SIZE,
			  queue->sq_buffer, queue->sq_dma_addr);
	dma_free_coherent(&dev->pdev->dev,
			  queue->cq_size * ACCEL_CQE_SIZE,
			  queue->cq_buffer, queue->cq_dma_addr);

	dev->queues[qid] = NULL;
	dev->num_queues--;
	kfree(queue);

	dev_dbg(&dev->pdev->dev, "Deleted queue %u\n", qid);

	return 0;
}
