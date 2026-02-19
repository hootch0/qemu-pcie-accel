// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe Accelerator Device - Character Device Interface with io_uring
 *
 * Copyright (C) 2026
 *
 * This file implements the character device interface for userspace with
 * full io_uring command passthrough support. Key features:
 *
 * - io_uring uring_cmd handler for low-latency async I/O
 * - Direct command passthrough via IORING_OP_URING_CMD
 * - Batched submission support with minimal syscall overhead
 * - Legacy IOCTL path for compatibility
 * - Direct doorbell access via mmap
 *
 * io_uring Integration:
 * The driver registers an uring_cmd callback that handles commands embedded
 * in io_uring SQEs. This allows userspace to submit device commands with:
 * 1. No syscall per command (batched submission)
 * 2. Zero-copy completion notification
 * 3. Async completion via io_uring CQEs
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/io_uring.h>
#include <linux/io_uring/cmd.h>
#include <linux/mm.h>

#include "pcie-accel.h"

/* External class reference */
extern struct class *accel_get_class(void);

/*
 * ===== File Operations =====
 */

/**
 * accel_open - Open device file
 * @inode: Inode structure
 * @file: File structure
 *
 * Returns: 0 on success
 */
static int accel_open(struct inode *inode, struct file *file)
{
	struct accel_dev *dev;

	dev = container_of(inode->i_cdev, struct accel_dev, cdev);
	file->private_data = dev;

	return 0;
}

/**
 * accel_release - Close device file
 * @inode: Inode structure
 * @file: File structure
 *
 * Returns: 0
 */
static int accel_release(struct inode *inode, struct file *file)
{
	/* Nothing to clean up on close */
	return 0;
}

/*
 * ===== io_uring Command Handler =====
 *
 * The uring_cmd callback is the core of io_uring integration. It receives
 * commands from userspace via IORING_OP_URING_CMD SQEs and processes them
 * asynchronously.
 *
 * io_uring SQE Layout for uring_cmd:
 *   sqe->cmd_op:   Operation code (0-7)
 *   sqe->cmd:      80-byte command data (struct accel_uring_cmd)
 *   sqe->fd:       File descriptor for the device
 *
 * The command data is parsed and dispatched to the appropriate handler.
 */

/*
 * ===== Scatter-Gather DMA Helpers =====
 *
 * NVMe-style scatter-gather: pin user pages, DMA-map each page, then
 * build either a PRP list (flags=0x00) or SGL descriptor array (flags=0x01).
 * DMA goes directly to/from user pages — no bounce buffer needed.
 */

/**
 * accel_setup_user_pages - Pin user pages and DMA-map them
 * @dev: Device structure
 * @user_addr: User virtual address of buffer
 * @data_len: Transfer length in bytes
 * @is_write: true if device writes to host (host receives data)
 * @sg: Output scatter-gather state
 *
 * Returns: 0 on success, negative errno on failure
 */
static int accel_setup_user_pages(struct accel_dev *dev, u64 user_addr,
				  size_t data_len, bool is_write,
				  struct accel_sg_state *sg)
{
	unsigned long start = user_addr & PAGE_MASK;
	unsigned int offset = user_addr & ~PAGE_MASK;
	int nr_pages = DIV_ROUND_UP(offset + data_len, PAGE_SIZE);
	unsigned int gup_flags = is_write ? FOLL_WRITE : 0;
	int i, pinned;

	memset(sg, 0, sizeof(*sg));

	sg->pages = kvmalloc_array(nr_pages, sizeof(struct page *), GFP_KERNEL);
	if (!sg->pages)
		return -ENOMEM;

	sg->dma_addrs = kvmalloc_array(nr_pages, sizeof(dma_addr_t), GFP_KERNEL);
	if (!sg->dma_addrs) {
		kvfree(sg->pages);
		sg->pages = NULL;
		return -ENOMEM;
	}

	pinned = pin_user_pages_fast(start, nr_pages, gup_flags, sg->pages);
	if (pinned < nr_pages) {
		if (pinned > 0)
			unpin_user_pages(sg->pages, pinned);
		kvfree(sg->dma_addrs);
		kvfree(sg->pages);
		sg->pages = NULL;
		sg->dma_addrs = NULL;
		return -EFAULT;
	}

	sg->nr_pages = nr_pages;
	sg->first_offset = offset;
	sg->total_len = data_len;
	sg->dir = is_write ? DMA_FROM_DEVICE : DMA_TO_DEVICE;

	/* DMA-map each page */
	for (i = 0; i < nr_pages; i++) {
		unsigned int pg_off = (i == 0) ? offset : 0;
		unsigned int pg_len;

		if (i == 0)
			pg_len = min_t(size_t, PAGE_SIZE - offset, data_len);
		else if (i == nr_pages - 1)
			pg_len = (offset + data_len - 1) % PAGE_SIZE + 1;
		else
			pg_len = PAGE_SIZE;

		sg->dma_addrs[i] = dma_map_page(&dev->pdev->dev, sg->pages[i],
						  pg_off, pg_len, sg->dir);
		if (dma_mapping_error(&dev->pdev->dev, sg->dma_addrs[i])) {
			/* Unmap previously mapped pages */
			while (--i >= 0) {
				unsigned int undo_off = (i == 0) ? offset : 0;
				unsigned int undo_len;

				if (i == 0)
					undo_len = min_t(size_t, PAGE_SIZE - offset, data_len);
				else
					undo_len = PAGE_SIZE;
				dma_unmap_page(&dev->pdev->dev, sg->dma_addrs[i],
					       undo_len, sg->dir);
			}
			unpin_user_pages(sg->pages, nr_pages);
			kvfree(sg->dma_addrs);
			kvfree(sg->pages);
			sg->pages = NULL;
			sg->dma_addrs = NULL;
			sg->nr_pages = 0;
			return -EIO;
		}
	}

	return 0;
}

/**
 * accel_setup_prp - Build NVMe-style PRP list from pinned pages
 * @dev: Device structure
 * @cmd: Command to modify (sets dbd.prpl.prp1 and prp2)
 * @sg: Scatter-gather state (pages must be pinned and DMA-mapped)
 *
 * NVMe PRP semantics:
 * - 1 page:  prp1 = page DMA addr + offset, prp2 = 0
 * - 2 pages: prp1 = page[0], prp2 = page[1] DMA addr
 * - >2 pages: prp1 = page[0], prp2 = PRP list DMA addr
 *   PRP list is a DMA buffer containing page[1..N] DMA addresses.
 *
 * Returns: 0 on success, negative errno on failure
 */
static int accel_setup_prp(struct accel_dev *dev, union accel_cmd *cmd,
			   struct accel_sg_state *sg)
{
	/* PRP1 = first page DMA addr (includes sub-page offset) */
	cmd->dbd.prpl.prp1 = cpu_to_le64(sg->dma_addrs[0]);

	if (sg->nr_pages == 1) {
		cmd->dbd.prpl.prp2 = 0;
		return 0;
	}

	if (sg->nr_pages == 2) {
		cmd->dbd.prpl.prp2 = cpu_to_le64(sg->dma_addrs[1]);
		return 0;
	}

	/* >2 pages: allocate PRP list buffer containing page[1..N] addrs */
	sg->prp_list = dma_alloc_coherent(&dev->pdev->dev, PAGE_SIZE,
					   &sg->prp_list_dma, GFP_ATOMIC);
	if (!sg->prp_list)
		return -ENOMEM;

	{
		int i;
		for (i = 1; i < sg->nr_pages; i++)
			sg->prp_list[i - 1] = cpu_to_le64(sg->dma_addrs[i]);
	}

	cmd->dbd.prpl.prp2 = cpu_to_le64(sg->prp_list_dma);
	return 0;
}

/**
 * accel_setup_sgl - Build NVMe-style SGL Last Segment from pinned pages
 * @dev: Device structure
 * @cmd: Command to modify (sets flags and dbd.sgl)
 * @sg: Scatter-gather state (pages must be pinned and DMA-mapped)
 *
 * Builds a single SGL Last Segment descriptor array with one Data Block
 * descriptor per pinned page. Sets command flags to SGL mode.
 *
 * Returns: 0 on success, negative errno on failure
 */
static int accel_setup_sgl(struct accel_dev *dev, union accel_cmd *cmd,
			   struct accel_sg_state *sg)
{
	size_t desc_size = sg->nr_pages * sizeof(struct accel_sgl_desc);
	int i;

	sg->sgl_descs_size = desc_size;
	sg->sgl_descs = dma_alloc_coherent(&dev->pdev->dev, desc_size,
					    &sg->sgl_descs_dma, GFP_ATOMIC);
	if (!sg->sgl_descs)
		return -ENOMEM;

	/* Fill one Data Block descriptor per page */
	for (i = 0; i < sg->nr_pages; i++) {
		unsigned int pg_len;

		if (i == 0)
			pg_len = min_t(size_t, PAGE_SIZE - sg->first_offset,
				       sg->total_len);
		else if (i == sg->nr_pages - 1)
			pg_len = (sg->first_offset + sg->total_len - 1) %
				 PAGE_SIZE + 1;
		else
			pg_len = PAGE_SIZE;

		sg->sgl_descs[i].addr = cpu_to_le64(sg->dma_addrs[i]);
		sg->sgl_descs[i].length = cpu_to_le32(pg_len);
		memset(sg->sgl_descs[i].reserved, 0, 3);
		sg->sgl_descs[i].type = ACCEL_SGL_DESC_DATA_BLOCK;
	}

	/* Set command to SGL mode: Last Segment pointing to descriptor array */
	cmd->flags = (cmd->flags & ~ACCEL_CMD_FLAGS_DBD_MASK) |
		     ACCEL_CMD_FLAGS_DBD_SGL;
	cmd->dbd.sgl.addr = cpu_to_le64(sg->sgl_descs_dma);
	cmd->dbd.sgl.length = cpu_to_le32(desc_size);
	memset(cmd->dbd.sgl.reserved, 0, 3);
	cmd->dbd.sgl.type = ACCEL_SGL_DESC_LAST_SEGMENT;

	return 0;
}

/**
 * accel_uring_cmd_submit - Handle ACCEL_URING_CMD_SUBMIT
 * @ioucmd: io_uring command context
 * @dev: Device structure
 * @ucmd: User command structure
 * @issue_flags: Issue flags from io_uring
 *
 * Submits a device command via io_uring. The command is processed
 * asynchronously and completion is delivered via io_uring CQE.
 *
 * Returns: -EIOCBQUEUED on success (async), negative error on failure
 */
static int accel_uring_cmd_submit(struct io_uring_cmd *ioucmd,
				  struct accel_dev *dev,
				  const struct accel_uring_cmd *ucmd,
				  unsigned int issue_flags)
{
	struct accel_queue *queue;
	union accel_cmd cmd;
	struct accel_sg_state sg;
	bool has_sg = false;
	size_t data_len = 0;
	u64 user_addr = 0;
	u16 qid = ucmd->qid;
	u8 dbd_type;
	bool is_host_write;  /* true = device writes to host (read op) */
	int ret;

	/* Validate queue ID */
	if (qid >= ACCEL_MAX_QUEUES)
		return -EINVAL;

	queue = dev->queues[qid];
	if (!queue)
		return -ENODEV;

	/* Copy command from userspace-provided structure */
	memcpy(&cmd, &ucmd->submit.cmd, sizeof(cmd));

	/*
	 * Determine data buffer requirements based on command opcode.
	 */
	switch (cmd.opcode) {
	case ACCEL_CMD_LOOPBACK:
		data_len = le32_to_cpu(cmd.dw.loopback.length);
		break;
	case ACCEL_CMD_P2P_WRITE:
	case ACCEL_CMD_P2P_READ:
		data_len = le32_to_cpu(cmd.dw.p2p.length);
		break;
	case ACCEL_CMD_MEM_READ:
	case ACCEL_CMD_MEM_WRITE:
		data_len = le32_to_cpu(cmd.mem_write.length);
		break;
	default:
		data_len = 0;
		break;
	}

	/*
	 * Set up scatter-gather DMA: pin user pages and build PRP/SGL
	 * descriptors. No bounce buffer — DMA goes directly to user pages.
	 */
	if (data_len > 0 && data_len <= (16 * 1024 * 1024)) {
		user_addr = le64_to_cpu(cmd.dbd.prpl.prp1);
		if (!user_addr || !access_ok((void __user *)user_addr, data_len))
			return -EFAULT;

		/*
		 * For host-write ops (P2P_READ, MEM_READ, LOOPBACK),
		 * device DMA-writes to user pages (FOLL_WRITE).
		 * For host-read ops (P2P_WRITE, MEM_WRITE),
		 * device DMA-reads from user pages.
		 */
		is_host_write = (cmd.opcode != ACCEL_CMD_P2P_WRITE &&
				 cmd.opcode != ACCEL_CMD_MEM_WRITE);

		ret = accel_setup_user_pages(dev, user_addr, data_len,
					     is_host_write, &sg);
		if (ret)
			return ret;

		has_sg = true;

		/* Build PRP list or SGL descriptors based on DBD flags */
		dbd_type = cmd.flags & ACCEL_CMD_FLAGS_DBD_MASK;
		if (dbd_type == ACCEL_CMD_FLAGS_DBD_SGL)
			ret = accel_setup_sgl(dev, &cmd, &sg);
		else
			ret = accel_setup_prp(dev, &cmd, &sg);

		if (ret) {
			accel_sg_cleanup(dev, &sg);
			return ret;
		}

		dev_dbg(&dev->pdev->dev,
			"SUBMIT: SG opcode=0x%02x nr_pages=%d dbd=%s\n",
			cmd.opcode, sg.nr_pages,
			dbd_type == ACCEL_CMD_FLAGS_DBD_SGL ? "SGL" : "PRP");
	}

	/*
	 * Submit command for async completion.
	 * Pass sg state — cleaned up by __accel_free_request() on completion.
	 */
	ret = accel_submit_async_cmd(queue, &cmd, ioucmd,
				     has_sg ? &sg : NULL,
				     NULL, 0, 0, NULL);
	if (ret) {
		dev_err(&dev->pdev->dev,
			"SUBMIT: accel_submit_async_cmd failed: %d\n", ret);
		if (has_sg)
			accel_sg_cleanup(dev, &sg);
		return ret;
	}

	/*
	 * Return -EIOCBQUEUED to tell io_uring that the command is in
	 * progress and completion will be delivered asynchronously.
	 */
	return -EIOCBQUEUED;
}

/**
 * accel_uring_cmd_create_queue - Handle ACCEL_URING_CMD_CREATE_QUEUE
 * @ioucmd: io_uring command context
 * @dev: Device structure
 * @ucmd: User command structure
 *
 * Creates a new I/O queue pair. This is a synchronous operation.
 *
 * Returns: 0 on success, negative error on failure
 */
static int accel_uring_cmd_create_queue(struct io_uring_cmd *ioucmd,
					struct accel_dev *dev,
					const struct accel_uring_cmd *ucmd)
{
	u16 qid = ucmd->qid;
	u16 sq_size = ucmd->create_queue.sq_size;
	u16 cq_size = ucmd->create_queue.cq_size;
	int ret;

	mutex_lock(&dev->dev_mutex);
	ret = accel_create_queue(dev, qid);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

/**
 * accel_uring_cmd_delete_queue - Handle ACCEL_URING_CMD_DELETE_QUEUE
 * @ioucmd: io_uring command context
 * @dev: Device structure
 * @ucmd: User command structure
 *
 * Deletes an existing I/O queue pair. This is a synchronous operation.
 *
 * Returns: 0 on success, negative error on failure
 */
static int accel_uring_cmd_delete_queue(struct io_uring_cmd *ioucmd,
					struct accel_dev *dev,
					const struct accel_uring_cmd *ucmd)
{
	u16 qid = ucmd->delete_queue.qid;
	int ret;

	mutex_lock(&dev->dev_mutex);
	ret = accel_delete_queue(dev, qid);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

/**
 * accel_uring_cmd_setup_p2p - Handle ACCEL_URING_CMD_SETUP_P2P
 * @ioucmd: io_uring command context
 * @dev: Device structure
 * @ucmd: User command structure
 *
 * Sets up a P2P peer device and ring buffer. This is a synchronous operation.
 *
 * Returns: 0 on success, negative error on failure
 */
static int accel_uring_cmd_setup_p2p(struct io_uring_cmd *ioucmd,
				     struct accel_dev *dev,
				     const struct accel_uring_cmd *ucmd)
{
	struct accel_p2p_ring_setup params = {
		.peer_bdf = ucmd->setup_p2p.peer_bdf,
		.slot = ucmd->setup_p2p.slot,
		.peer_slot = ucmd->setup_p2p.peer_slot,
		.peer_bar0 = ucmd->setup_p2p.peer_bar0,
	};
	int ret;

	mutex_lock(&dev->dev_mutex);
	ret = accel_setup_p2p_peer(dev, &params);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

/**
 * accel_uring_cmd_get_stats - Handle ACCEL_URING_CMD_GET_STATS
 * @ioucmd: io_uring command context
 * @dev: Device structure
 * @ucmd: User command structure
 *
 * Returns device statistics. Result is returned in io_uring CQE res field.
 *
 * Returns: 0 on success (stats in big_cqe), negative error on failure
 */
static int accel_uring_cmd_get_stats(struct io_uring_cmd *ioucmd,
				     struct accel_dev *dev,
				     const struct accel_uring_cmd *ucmd)
{
	/*
	 * Return basic stats in the CQE res field.
	 * For detailed stats, userspace should use IOCTL.
	 */
	u64 cmd_submitted = atomic64_read(&dev->cmd_submitted);
	u64 uring_submissions = atomic64_read(&dev->uring_submissions);

	/* Pack into result: high 32 bits = uring submissions, low = total */
	return (int)((uring_submissions & 0xFFFF) << 16 |
		     (cmd_submitted & 0xFFFF));
}

/**
 * accel_uring_cmd - Main io_uring command handler
 * @ioucmd: io_uring command context
 * @issue_flags: Flags from io_uring (e.g., IO_URING_F_NONBLOCK)
 *
 * This is the main entry point for io_uring commands. It's called
 * when userspace submits an IORING_OP_URING_CMD SQE for this device.
 *
 * The command data is embedded in the SQE's cmd field (80 bytes).
 * We parse it as struct accel_uring_cmd and dispatch to the
 * appropriate handler.
 *
 * Returns:
 *   -EIOCBQUEUED: Command queued for async completion
 *   0 or positive: Synchronous completion (value goes in CQE.res)
 *   negative: Error code
 */
int accel_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags)
{
	struct file *file = ioucmd->file;
	struct accel_dev *dev = file->private_data;
	const struct accel_uring_cmd *ucmd;

	/*
	 * Get pointer to command data in the SQE.
	 * io_uring_sqe_cmd() returns the 80-byte cmd field.
	 */
	ucmd = io_uring_sqe_cmd(ioucmd->sqe);

	/* Dispatch based on operation code */
	switch (ucmd->op) {
	case ACCEL_URING_CMD_SUBMIT:
		return accel_uring_cmd_submit(ioucmd, dev, ucmd, issue_flags);

	case ACCEL_URING_CMD_CREATE_QUEUE:
		return accel_uring_cmd_create_queue(ioucmd, dev, ucmd);

	case ACCEL_URING_CMD_DELETE_QUEUE:
		return accel_uring_cmd_delete_queue(ioucmd, dev, ucmd);

	case ACCEL_URING_CMD_SETUP_P2P:
		return accel_uring_cmd_setup_p2p(ioucmd, dev, ucmd);

	case ACCEL_URING_CMD_GET_STATS:
		return accel_uring_cmd_get_stats(ioucmd, dev, ucmd);

	case ACCEL_URING_CMD_ADMIN:
		/*
		 * Admin commands go through the admin queue (qid=0).
		 * Force qid to 0 and use submit path.
		 */
		{
			struct accel_uring_cmd admin_ucmd = *ucmd;
			admin_ucmd.qid = 0;
			return accel_uring_cmd_submit(ioucmd, dev, &admin_ucmd,
						      issue_flags);
		}

	default:
		return -EINVAL;
	}
}

/*
 * ===== Legacy IOCTL Handlers =====
 *
 * These provide backward compatibility for applications not using io_uring.
 */

/**
 * accel_ioctl_create_queue - Create queue IOCTL handler
 */
static long accel_ioctl_create_queue(struct accel_dev *dev, unsigned long arg)
{
	struct accel_uring_cmd qc;
	int ret;

	if (copy_from_user(&qc, (void __user *)arg, sizeof(qc)))
		return -EFAULT;

	mutex_lock(&dev->dev_mutex);
	ret = accel_create_queue(dev, qc.qid);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

/**
 * accel_ioctl_delete_queue - Delete queue IOCTL handler
 */
static long accel_ioctl_delete_queue(struct accel_dev *dev, unsigned long arg)
{
	u16 qid = (u16)arg;
	int ret;

	mutex_lock(&dev->dev_mutex);
	ret = accel_delete_queue(dev, qid);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

/**
 * accel_ioctl_submit_cmd - Submit command IOCTL handler (synchronous)
 *
 * Uses scatter-gather DMA: pins user pages, builds PRP/SGL descriptors,
 * submits synchronously, then cleans up on completion.
 */
static long accel_ioctl_submit_cmd(struct accel_dev *dev, unsigned long arg)
{
	struct accel_uring_cmd ucmd;
	struct accel_queue *queue;
	struct accel_cqe cqe;
	struct accel_sg_state sg;
	bool has_sg = false;
	u32 data_len;
	u64 user_addr;
	u8 dbd_type;
	bool is_host_write;
	int ret;

	if (copy_from_user(&ucmd, (void __user *)arg, sizeof(ucmd)))
		return -EFAULT;

	/* Validate queue */
	if (ucmd.qid >= ACCEL_MAX_QUEUES)
		return -EINVAL;

	queue = dev->queues[ucmd.qid];
	if (!queue)
		return -ENODEV;

	/* Determine data buffer size */
	switch (ucmd.submit.cmd.opcode) {
	case ACCEL_CMD_LOOPBACK:
		data_len = le32_to_cpu(ucmd.submit.cmd.dw.loopback.length);
		break;
	case ACCEL_CMD_P2P_WRITE:
	case ACCEL_CMD_P2P_READ:
		data_len = le32_to_cpu(ucmd.submit.cmd.dw.p2p.length);
		break;
	case ACCEL_CMD_MEM_READ:
	case ACCEL_CMD_MEM_WRITE:
		data_len = le32_to_cpu(ucmd.submit.cmd.mem_write.length);
		break;
	default:
		data_len = 0;
		break;
	}

	/* Set up scatter-gather DMA for data commands */
	if (data_len > 0 && data_len <= (16 * 1024 * 1024)) {
		user_addr = le64_to_cpu(ucmd.submit.cmd.dbd.prpl.prp1);
		if (!user_addr || !access_ok((void __user *)user_addr, data_len))
			return -EFAULT;

		is_host_write = (ucmd.submit.cmd.opcode != ACCEL_CMD_P2P_WRITE &&
				 ucmd.submit.cmd.opcode != ACCEL_CMD_MEM_WRITE);

		ret = accel_setup_user_pages(dev, user_addr, data_len,
					     is_host_write, &sg);
		if (ret)
			return ret;

		has_sg = true;

		dbd_type = ucmd.submit.cmd.flags & ACCEL_CMD_FLAGS_DBD_MASK;
		if (dbd_type == ACCEL_CMD_FLAGS_DBD_SGL)
			ret = accel_setup_sgl(dev, &ucmd.submit.cmd, &sg);
		else
			ret = accel_setup_prp(dev, &ucmd.submit.cmd, &sg);

		if (ret) {
			accel_sg_cleanup(dev, &sg);
			return ret;
		}
	}

	/* Submit synchronously */
	ret = accel_submit_sync_cmd(dev, ucmd.qid, &ucmd.submit.cmd, &cqe,
				    ucmd.timeout_ms ? ucmd.timeout_ms : 5000);

	/* Clean up scatter-gather state */
	if (has_sg)
		accel_sg_cleanup(dev, &sg);

	return ret;
}

/**
 * accel_ioctl_setup_p2p - Setup P2P peer IOCTL handler
 */
static long accel_ioctl_setup_p2p(struct accel_dev *dev, unsigned long arg)
{
	struct accel_uring_cmd ucmd;
	struct accel_p2p_ring_setup params;
	int ret;

	if (copy_from_user(&ucmd, (void __user *)arg, sizeof(ucmd)))
		return -EFAULT;

	params.peer_bdf = ucmd.setup_p2p.peer_bdf;
	params.slot = ucmd.setup_p2p.slot;
	params.peer_slot = ucmd.setup_p2p.peer_slot;
	params.peer_bar0 = ucmd.setup_p2p.peer_bar0;

	mutex_lock(&dev->dev_mutex);
	ret = accel_setup_p2p_peer(dev, &params);
	mutex_unlock(&dev->dev_mutex);

	return ret;
}

/**
 * accel_ioctl_get_stats - Get statistics IOCTL handler
 */
static long accel_ioctl_get_stats(struct accel_dev *dev, unsigned long arg)
{
	struct accel_uring_result stats;
	struct accel_p2p_peer *peer;
	unsigned long flags;

	memset(&stats, 0, sizeof(stats));

	stats.result = atomic64_read(&dev->cmd_submitted);
	stats.sq_head = atomic64_read(&dev->uring_submissions);
	stats.sq_id = atomic64_read(&dev->uring_completions);
	stats.dev_status = dev->num_queues;

	/* Count P2P peers */
	spin_lock_irqsave(&dev->p2p_lock, flags);
	stats.cid = 0;
	list_for_each_entry(peer, &dev->p2p_peers, list)
		stats.cid++;
	spin_unlock_irqrestore(&dev->p2p_lock, flags);

	if (copy_to_user((void __user *)arg, &stats, sizeof(stats)))
		return -EFAULT;

	return 0;
}

/**
 * accel_ioctl - Main IOCTL handler
 */
static long accel_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct accel_dev *dev = file->private_data;

	switch (cmd) {
	case ACCEL_IOC_CREATE_QUEUE:
		return accel_ioctl_create_queue(dev, arg);

	case ACCEL_IOC_DELETE_QUEUE:
		return accel_ioctl_delete_queue(dev, arg);

	case ACCEL_IOC_SUBMIT_CMD:
		return accel_ioctl_submit_cmd(dev, arg);

	case ACCEL_IOC_SETUP_P2P:
		return accel_ioctl_setup_p2p(dev, arg);

	case ACCEL_IOC_GET_STATS:
		return accel_ioctl_get_stats(dev, arg);

	case ACCEL_IOC_P2P_RING_SETUP: {
		struct accel_p2p_ring_setup params;
		if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
			return -EFAULT;
		return accel_setup_p2p_peer(dev, &params);
	}

	default:
		return -ENOTTY;
	}
}

/*
 * ===== Memory Mapping =====
 */

/**
 * accel_mmap - Map device memory to user space
 * @file: File structure
 * @vma: Virtual memory area
 *
 * Allows mapping of device BAR0 for direct doorbell access.
 * This enables ultra-low-latency command submission by allowing
 * userspace to write doorbells directly without kernel involvement.
 *
 * Mapping offset determines what to map:
 *   0: Full BAR0 (registers + doorbells)
 *   0x1000: Just the doorbell region
 *
 * Returns: 0 on success, negative error code on failure
 */
static int accel_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct accel_dev *dev = file->private_data;
	struct pci_dev *pdev = dev->pdev;
	unsigned long size = vma->vm_end - vma->vm_start;
	resource_size_t bar_start = pci_resource_start(pdev, 0);
	resource_size_t bar_size = pci_resource_len(pdev, 0);
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	/* Validate mapping request */
	if (offset + size > bar_size)
		return -EINVAL;

	/*
	 * Set memory type to uncached for MMIO.
	 * This ensures writes are not cached and are immediately
	 * visible to the device.
	 */
	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	/* Set VM flags for MMIO mapping */
	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);

	/* Map the physical pages to userspace */
	if (io_remap_pfn_range(vma, vma->vm_start,
			       (bar_start + offset) >> PAGE_SHIFT, size,
			       vma->vm_page_prot))
		return -EAGAIN;

	return 0;
}

/*
 * ===== File Operations Structure =====
 *
 * The uring_cmd callback is the key addition for io_uring support.
 */
static const struct file_operations accel_fops = {
	.owner		= THIS_MODULE,
	.open		= accel_open,
	.release	= accel_release,
	.unlocked_ioctl	= accel_ioctl,
	.compat_ioctl	= accel_ioctl,
	.mmap		= accel_mmap,
	.uring_cmd	= accel_uring_cmd,
	.uring_cmd_iopoll = NULL,  /* Polling not supported */
};

/*
 * ===== Character Device Setup =====
 */

/**
 * accel_setup_chardev - Create character device with io_uring support
 * @dev: Device structure
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_setup_chardev(struct accel_dev *dev)
{
	int ret;
	struct class *accel_class = accel_get_class();

	/* Initialize cdev with our file operations */
	cdev_init(&dev->cdev, &accel_fops);
	dev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&dev->cdev, dev->devt, 1);
	if (ret) {
		dev_err(&dev->pdev->dev, "Failed to add cdev: %d\n", ret);
		return ret;
	}

	/* Create device node in /dev */
	dev->char_dev = device_create(accel_class, &dev->pdev->dev,
				      dev->devt, dev, "accel%d",
				      MINOR(dev->devt));
	if (IS_ERR(dev->char_dev)) {
		ret = PTR_ERR(dev->char_dev);
		dev_err(&dev->pdev->dev, "Failed to create device: %d\n", ret);
		cdev_del(&dev->cdev);
		return ret;
	}

	dev_info(&dev->pdev->dev,
		 "Character device created: /dev/accel%d (io_uring enabled)\n",
		 MINOR(dev->devt));

	return 0;
}

/**
 * accel_cleanup_chardev - Destroy character device
 * @dev: Device structure
 */
void accel_cleanup_chardev(struct accel_dev *dev)
{
	struct class *accel_class = accel_get_class();

	if (dev->char_dev) {
		device_destroy(accel_class, dev->devt);
		dev->char_dev = NULL;
	}

	cdev_del(&dev->cdev);
}
