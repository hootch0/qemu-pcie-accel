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
	struct accel_cmd cmd;
	dma_addr_t data_dma = 0;
	void *data_buf = NULL;
	void __user *user_buf = NULL;
	size_t data_len = 0;
	u16 qid = ucmd->qid;
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
	 * Different commands have different data layouts.
	 */
	switch (cmd.opcode) {
	case ACCEL_CMD_LOOPBACK:
		data_len = le32_to_cpu(cmd.dw.loopback.length);
		dev_dbg(&dev->pdev->dev, "SUBMIT: LOOPBACK qid=%u len=%zu\n",
			qid, data_len);
		break;
	case ACCEL_CMD_P2P_WRITE:
	case ACCEL_CMD_P2P_READ:
		data_len = le32_to_cpu(cmd.dw.p2p.length);
		dev_dbg(&dev->pdev->dev,
			"SUBMIT: P2P_%s qid=%u peer_bdf=0x%x peer_addr=0x%llx "
			"prp1=0x%llx len=%zu\n",
			cmd.opcode == ACCEL_CMD_P2P_WRITE ? "WRITE" : "READ",
			qid, le32_to_cpu(cmd.dw.p2p.peer_bdf),
			le64_to_cpu(cmd.dw.p2p.peer_addr),
			le64_to_cpu(cmd.prp1), data_len);
		break;
	case ACCEL_CMD_CXL_READ:
	case ACCEL_CMD_CXL_WRITE:
		data_len = le32_to_cpu(cmd.dw.cxl.length);
		break;
	default:
		/* No data buffer needed */
		data_len = 0;
		break;
	}

	/* Allocate DMA buffer if needed (limit to 16MB) */
	if (data_len > 0 && data_len <= (16 * 1024 * 1024)) {
		u64 user_addr = le64_to_cpu(cmd.prp1);

		data_buf = dma_alloc_coherent(&dev->pdev->dev, data_len,
					      &data_dma, GFP_ATOMIC);
		if (!data_buf)
			return -ENOMEM;

		/*
		 * For write operations, copy data from user-provided address.
		 * For read operations, save user address for copy back on completion.
		 */
		if (cmd.opcode == ACCEL_CMD_P2P_WRITE ||
		    cmd.opcode == ACCEL_CMD_CXL_WRITE ||
		    cmd.opcode == ACCEL_CMD_LOOPBACK) {
			void __user *uptr = (void __user *)user_addr;

			/* Validate user address before copying */
			if (!user_addr || !access_ok(uptr, data_len)) {
				dma_free_coherent(&dev->pdev->dev, data_len,
						  data_buf, data_dma);
				return -EFAULT;
			}

			if (copy_from_user(data_buf, uptr, data_len)) {
				dma_free_coherent(&dev->pdev->dev, data_len,
						  data_buf, data_dma);
				return -EFAULT;
			}
		} else if (cmd.opcode == ACCEL_CMD_P2P_READ ||
			   cmd.opcode == ACCEL_CMD_CXL_READ) {
			/* Save user buffer for copy back on completion */
			if (!user_addr || !access_ok((void __user *)user_addr, data_len)) {
				dma_free_coherent(&dev->pdev->dev, data_len,
						  data_buf, data_dma);
				return -EFAULT;
			}
			user_buf = (void __user *)user_addr;
		}

		/* Replace user address with DMA address */
		cmd.prp1 = cpu_to_le64(data_dma);

		dev_dbg(&dev->pdev->dev,
			"SUBMIT: DMA buf=%p dma_addr=0x%llx len=%zu user_buf=%p\n",
			data_buf, (u64)data_dma, data_len, user_buf);
	}

	/*
	 * Submit command for async completion.
	 * accel_submit_async_cmd() will track the request and deliver
	 * completion via io_uring_cmd_done() when the device completes.
	 * For read operations, user_buf is saved for copy back on completion.
	 */
	ret = accel_submit_async_cmd(queue, &cmd, ioucmd, data_buf,
				     data_dma, data_len, user_buf);
	if (ret) {
		dev_err(&dev->pdev->dev,
			"SUBMIT: accel_submit_async_cmd failed: %d\n", ret);
		if (data_buf)
			dma_free_coherent(&dev->pdev->dev, data_len,
					  data_buf, data_dma);
		return ret;
	}

	dev_dbg(&dev->pdev->dev,
		"SUBMIT: queued opcode=%u cid=%u to qid=%u\n",
		cmd.opcode, le16_to_cpu(cmd.cid), qid);

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
	ret = accel_create_queue(dev, qid, sq_size, cq_size);
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
 * Sets up a P2P peer device. This is a synchronous operation.
 *
 * Returns: 0 on success, negative error on failure
 */
static int accel_uring_cmd_setup_p2p(struct io_uring_cmd *ioucmd,
				     struct accel_dev *dev,
				     const struct accel_uring_cmd *ucmd)
{
	u16 peer_bdf = ucmd->setup_p2p.peer_bdf;
	int ret;

	mutex_lock(&dev->dev_mutex);
	ret = accel_setup_p2p_peer(dev, peer_bdf);
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
	ret = accel_create_queue(dev, qc.qid,
				 qc.create_queue.sq_size,
				 qc.create_queue.cq_size);
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
 */
static long accel_ioctl_submit_cmd(struct accel_dev *dev, unsigned long arg)
{
	struct accel_uring_cmd ucmd;
	struct accel_queue *queue;
	struct accel_cqe cqe;
	dma_addr_t data_dma = 0;
	void *data_buf = NULL;
	u32 data_len;
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
	case ACCEL_CMD_CXL_READ:
	case ACCEL_CMD_CXL_WRITE:
		data_len = le32_to_cpu(ucmd.submit.cmd.dw.cxl.length);
		break;
	default:
		data_len = 0;
		break;
	}

	if (data_len > 0 && data_len <= (16 * 1024 * 1024)) {
		data_buf = dma_alloc_coherent(&dev->pdev->dev, data_len,
					      &data_dma, GFP_KERNEL);
		if (!data_buf)
			return -ENOMEM;

		/* Copy data for write operations */
		if (ucmd.submit.cmd.opcode == ACCEL_CMD_P2P_WRITE ||
		    ucmd.submit.cmd.opcode == ACCEL_CMD_CXL_WRITE ||
		    ucmd.submit.cmd.opcode == ACCEL_CMD_LOOPBACK) {
			u64 user_addr = le64_to_cpu(ucmd.submit.cmd.prp1);
			if (copy_from_user(data_buf, (void __user *)user_addr,
					   data_len)) {
				ret = -EFAULT;
				goto out_free;
			}
		}

		ucmd.submit.cmd.prp1 = cpu_to_le64(data_dma);
	}

	/* Submit synchronously */
	ret = accel_submit_sync_cmd(dev, ucmd.qid, &ucmd.submit.cmd, &cqe,
				    ucmd.timeout_ms ? ucmd.timeout_ms : 5000);

	if (ret)
		goto out_free;

	/* Copy back data for read operations */
	if (data_buf && (ucmd.submit.cmd.opcode == ACCEL_CMD_P2P_READ ||
			 ucmd.submit.cmd.opcode == ACCEL_CMD_CXL_READ ||
			 ucmd.submit.cmd.opcode == ACCEL_CMD_LOOPBACK)) {
		struct accel_uring_cmd orig;
		void __user *uptr;

		if (copy_from_user(&orig, (void __user *)arg, sizeof(orig))) {
			ret = -EFAULT;
			goto out_free;
		}
		u64 user_addr = le64_to_cpu(orig.submit.cmd.prp1);
		uptr = (void __user *)user_addr;

		/* Validate user address before copying */
		if (!user_addr || !access_ok(uptr, data_len)) {
			ret = -EFAULT;
			goto out_free;
		}

		if (copy_to_user(uptr, data_buf, data_len)) {
			ret = -EFAULT;
			goto out_free;
		}
	}

	ret = 0;

out_free:
	if (data_buf)
		dma_free_coherent(&dev->pdev->dev, data_len, data_buf, data_dma);

	return ret;
}

/**
 * accel_ioctl_setup_p2p - Setup P2P peer IOCTL handler
 */
static long accel_ioctl_setup_p2p(struct accel_dev *dev, unsigned long arg)
{
	struct accel_uring_cmd ucmd;
	int ret;

	if (copy_from_user(&ucmd, (void __user *)arg, sizeof(ucmd)))
		return -EFAULT;

	mutex_lock(&dev->dev_mutex);
	ret = accel_setup_p2p_peer(dev, ucmd.setup_p2p.peer_bdf);
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
