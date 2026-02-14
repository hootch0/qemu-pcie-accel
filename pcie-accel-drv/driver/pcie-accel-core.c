// SPDX-License-Identifier: GPL-2.0-only
/*
 * PCIe Accelerator Device - Core Driver with io_uring Support
 *
 * Copyright (C) 2026
 *
 * This file implements the core Linux kernel driver for the PCIe Accelerator
 * device with high-performance io_uring command passthrough. It handles:
 * - PCI device probe/remove
 * - MSI-X interrupt setup with per-queue vectors
 * - Admin queue initialization
 * - Controller enable/disable
 * - Request slab cache for efficient async tracking
 * - io_uring statistics and completion handling
 *
 * io_uring Integration:
 * The driver supports io_uring command passthrough (uring_cmd) for low-latency
 * async I/O. Commands are submitted via io_uring SQEs and completed through
 * CQEs, eliminating syscall overhead for batched operations.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/io_uring.h>

#include "pcie-accel.h"

/* Module parameters */
static unsigned int max_queues = 64;
module_param(max_queues, uint, 0444);
MODULE_PARM_DESC(max_queues, "Maximum number of I/O queues per device (default: 64)");

static unsigned int admin_queue_size = 64;
module_param(admin_queue_size, uint, 0444);
MODULE_PARM_DESC(admin_queue_size, "Admin queue size in entries (default: 64)");

static unsigned int req_pool_size = 256;
module_param(req_pool_size, uint, 0444);
MODULE_PARM_DESC(req_pool_size, "Request pool size per queue (default: 256)");

/* PCI device IDs */
static const struct pci_device_id accel_pci_tbl[] = {
	{ PCI_DEVICE(0x1234, 0x5678) },  /* QEMU PCIe Accelerator */
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, accel_pci_tbl);

/* Global driver state */
static struct class *accel_class;
static dev_t accel_devt;  /* Base device number (major + first minor) */
static DEFINE_IDA(accel_ida);

/*
 * ===== Register Access Helpers =====
 * These provide safe access to device MMIO registers with memory barriers.
 * All register accesses use readl/writel which include proper barriers.
 */

/**
 * accel_wait_ready - Wait for controller to become ready
 * @dev: Device structure
 * @timeout_ms: Timeout in milliseconds
 *
 * Polls CSTS.RDY (Controller Status Ready bit) until it becomes set.
 * This indicates the controller has completed initialization and is
 * ready to process commands.
 *
 * Controller Status Register (CSTS) at offset 0x1C:
 *   Bit 0 (RDY): Ready - Controller is ready when set
 *   Bit 1 (CFS): Controller Fatal Status - Fatal error when set
 *
 * Returns: 0 on success, -ETIMEDOUT on timeout, -EIO on fatal error
 */
static int accel_wait_ready(struct accel_dev *dev, u32 timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	u32 csts;

	do {
		csts = accel_reg_read32(dev, ACCEL_REG_CSTS);

		/* Check for Ready bit */
		if (csts & (1 << 0))
			return 0;

		/* Check for fatal error - controller cannot recover */
		if (csts & (1 << 1)) {
			dev_err(&dev->pdev->dev,
				"Controller fatal status set during enable\n");
			return -EIO;
		}

		/* Sleep to avoid busy-waiting, allows other tasks to run */
		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));

	dev_err(&dev->pdev->dev, "Timeout waiting for controller ready\n");
	return -ETIMEDOUT;
}

/**
 * accel_wait_not_ready - Wait for controller to become not ready
 * @dev: Device structure
 * @timeout_ms: Timeout in milliseconds
 *
 * Polls CSTS.RDY until it clears, indicating the controller has
 * completed shutdown.
 *
 * Returns: 0 on success, -ETIMEDOUT on timeout
 */
static int accel_wait_not_ready(struct accel_dev *dev, u32 timeout_ms)
{
	unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
	u32 csts;

	do {
		csts = accel_reg_read32(dev, ACCEL_REG_CSTS);
		if (!(csts & (1 << 0)))  /* RDY bit clear */
			return 0;

		usleep_range(1000, 2000);
	} while (time_before(jiffies, deadline));

	dev_err(&dev->pdev->dev, "Timeout waiting for controller not ready\n");
	return -ETIMEDOUT;
}

/*
 * ===== Request Slab Cache Management =====
 *
 * For efficient io_uring async operations, we use a slab cache to allocate
 * request tracking structures. This provides:
 * - Fast allocation/deallocation
 * - Memory efficiency through object reuse
 * - Cache-line alignment for performance
 */

/**
 * accel_create_req_cache - Create request slab cache for a device
 * @dev: Device structure
 *
 * Creates a dedicated slab cache for accel_request structures.
 * Each request tracks an in-flight async command through io_uring.
 *
 * Returns: 0 on success, -ENOMEM on failure
 */
static int accel_create_req_cache(struct accel_dev *dev)
{
	char cache_name[32];

	snprintf(cache_name, sizeof(cache_name), "accel%d_req",
		 MINOR(dev->devt));

	/*
	 * Create slab cache with:
	 * - Cache-line alignment for performance
	 * - SLAB_HWCACHE_ALIGN for CPU cache efficiency
	 */
	dev->req_cache = kmem_cache_create(cache_name,
					   sizeof(struct accel_request),
					   0,
					   SLAB_HWCACHE_ALIGN,
					   NULL);
	if (!dev->req_cache) {
		dev_err(&dev->pdev->dev, "Failed to create request cache\n");
		return -ENOMEM;
	}

	return 0;
}

/**
 * accel_destroy_req_cache - Destroy request slab cache
 * @dev: Device structure
 */
static void accel_destroy_req_cache(struct accel_dev *dev)
{
	if (dev->req_cache) {
		kmem_cache_destroy(dev->req_cache);
		dev->req_cache = NULL;
	}
}

/*
 * ===== MSI-X Setup =====
 *
 * MSI-X provides per-queue interrupt vectors for efficient completion
 * processing without shared interrupt handling overhead.
 */

/**
 * accel_setup_msix - Set up MSI-X interrupts
 * @dev: Device structure
 *
 * Allocates MSI-X vectors for admin queue and I/O queues.
 * Vector assignment:
 *   Vector 0: Admin queue completions
 *   Vector 1..N: I/O queue completions (one per queue)
 *
 * MSI-X enables efficient interrupt handling by:
 * - Eliminating shared interrupt overhead
 * - Allowing per-CPU interrupt affinity
 * - Supporting interrupt coalescing per vector
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_setup_msix(struct accel_dev *dev)
{
	int ret, i;
	int nr_vectors;

	/*
	 * Request one vector for admin queue plus one per I/O queue pair.
	 * We'll scale down if the device doesn't support that many.
	 * Maximum of 256 vectors per PCIe spec.
	 */
	nr_vectors = min_t(int, max_queues + 1, 256);

	dev->msix_entries = kcalloc(nr_vectors, sizeof(struct msix_entry),
				    GFP_KERNEL);
	if (!dev->msix_entries)
		return -ENOMEM;

	/* Initialize vector entries */
	for (i = 0; i < nr_vectors; i++)
		dev->msix_entries[i].entry = i;

	/*
	 * pci_enable_msix_range() allocates between min_vecs and max_vecs
	 * vectors. We need at least 1 (admin queue), but prefer more.
	 */
	ret = pci_enable_msix_range(dev->pdev, dev->msix_entries,
				    1, nr_vectors);
	if (ret < 0) {
		dev_err(&dev->pdev->dev, "Failed to enable MSI-X: %d\n", ret);
		kfree(dev->msix_entries);
		dev->msix_entries = NULL;
		return ret;
	}

	dev->num_vecs = ret;
	dev_info(&dev->pdev->dev, "Allocated %d MSI-X vectors\n", dev->num_vecs);

	return 0;
}

/**
 * accel_free_msix - Free MSI-X resources
 * @dev: Device structure
 */
void accel_free_msix(struct accel_dev *dev)
{
	if (dev->msix_entries) {
		pci_disable_msix(dev->pdev);
		kfree(dev->msix_entries);
		dev->msix_entries = NULL;
	}
	dev->num_vecs = 0;
}

/*
 * ===== Admin Queue Management =====
 *
 * The admin queue (QID 0) is used for device management operations:
 * - Creating/deleting I/O queues
 * - Identify commands
 * - P2P setup
 * - Feature configuration
 */

/**
 * accel_init_admin_queue - Initialize admin queue pair
 * @dev: Device structure
 *
 * Allocates DMA memory for admin SQ and CQ, configures the device registers,
 * and sets up the interrupt handler.
 *
 * Admin Queue Attributes Register (AQA) at offset 0x24:
 *   Bits [11:0]:  ASQS - Admin Submission Queue Size (0-based)
 *   Bits [27:16]: ACQS - Admin Completion Queue Size (0-based)
 *
 * Admin SQ Base Address (ASQ) at offset 0x28: 64-bit DMA address
 * Admin CQ Base Address (ACQ) at offset 0x30: 64-bit DMA address
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_init_admin_queue(struct accel_dev *dev)
{
	struct accel_queue *queue;
	int ret;

	queue = kzalloc(sizeof(*queue), GFP_KERNEL);
	if (!queue)
		return -ENOMEM;

	queue->dev = dev;
	queue->qid = 0;  /* Admin queue is always QID 0 */
	queue->sq_size = admin_queue_size;
	queue->cq_size = admin_queue_size;
	queue->cq_phase = 1;  /* Initial phase bit is 1 */

	spin_lock_init(&queue->sq_lock);
	spin_lock_init(&queue->cq_lock);

	/* Initialize request tracking hash table */
	hash_init(queue->req_hash);
	INIT_LIST_HEAD(&queue->req_free);
	INIT_LIST_HEAD(&queue->req_pending);
	queue->req_pool_size = admin_queue_size;

	/*
	 * Allocate submission queue buffer.
	 * Must be physically contiguous and accessible by device DMA.
	 * Queues require 4KB alignment minimum.
	 */
	queue->sq_buffer = dma_alloc_coherent(&dev->pdev->dev,
					      queue->sq_size * ACCEL_SQE_SIZE,
					      &queue->sq_dma_addr,
					      GFP_KERNEL);
	if (!queue->sq_buffer) {
		dev_err(&dev->pdev->dev, "Failed to allocate admin SQ buffer\n");
		ret = -ENOMEM;
		goto err_free_queue;
	}

	/*
	 * Allocate completion queue buffer.
	 * CQEs are written by device, read by host.
	 */
	queue->cq_buffer = dma_alloc_coherent(&dev->pdev->dev,
					      queue->cq_size * ACCEL_CQE_SIZE,
					      &queue->cq_dma_addr,
					      GFP_KERNEL);
	if (!queue->cq_buffer) {
		dev_err(&dev->pdev->dev, "Failed to allocate admin CQ buffer\n");
		ret = -ENOMEM;
		goto err_free_sq;
	}

	/* Clear queue buffers - important for phase bit detection */
	memset(queue->sq_buffer, 0, queue->sq_size * ACCEL_SQE_SIZE);
	memset(queue->cq_buffer, 0, queue->cq_size * ACCEL_CQE_SIZE);

	/* Set admin queue base addresses (64-bit DMA addresses) */
	accel_reg_write64(dev, ACCEL_REG_ASQ, queue->sq_dma_addr);
	accel_reg_write64(dev, ACCEL_REG_ACQ, queue->cq_dma_addr);

	/* Request interrupt for admin queue (vector 0) */
	if (dev->num_vecs > 0) {
		queue->irq_vector = dev->msix_entries[0].vector;
		ret = request_threaded_irq(queue->irq_vector,
					   accel_irq_handler,
					   accel_irq_handler_threaded,
					   0, "pcie-accel-admin", queue);
		if (ret) {
			dev_err(&dev->pdev->dev,
				"Failed to request admin IRQ: %d\n", ret);
			goto err_free_cq;
		}
	}

	dev->queues[0] = queue;
	dev->num_queues = 1;

	dev_dbg(&dev->pdev->dev,
		"Admin queue initialized: SQ@0x%llx CQ@0x%llx size=%u\n",
		queue->sq_dma_addr, queue->cq_dma_addr, queue->sq_size);

	return 0;

err_free_cq:
	dma_free_coherent(&dev->pdev->dev,
			  queue->cq_size * ACCEL_CQE_SIZE,
			  queue->cq_buffer, queue->cq_dma_addr);
err_free_sq:
	dma_free_coherent(&dev->pdev->dev,
			  queue->sq_size * ACCEL_SQE_SIZE,
			  queue->sq_buffer, queue->sq_dma_addr);
err_free_queue:
	kfree(queue);
	return ret;
}

/**
 * accel_free_admin_queue - Free admin queue resources
 * @dev: Device structure
 *
 * Releases all resources associated with the admin queue including
 * IRQ, DMA buffers, and the queue structure itself.
 */
void accel_free_admin_queue(struct accel_dev *dev)
{
	struct accel_queue *queue = dev->queues[0];

	if (!queue)
		return;

	/* Free interrupt handler */
	if (queue->irq_vector)
		free_irq(queue->irq_vector, queue);

	/* Free DMA buffers */
	if (queue->sq_buffer)
		dma_free_coherent(&dev->pdev->dev,
				  queue->sq_size * ACCEL_SQE_SIZE,
				  queue->sq_buffer, queue->sq_dma_addr);

	if (queue->cq_buffer)
		dma_free_coherent(&dev->pdev->dev,
				  queue->cq_size * ACCEL_CQE_SIZE,
				  queue->cq_buffer, queue->cq_dma_addr);

	/* Free request pool if allocated */
	if (queue->req_pool)
		kfree(queue->req_pool);

	kfree(queue);
	dev->queues[0] = NULL;
}

/*
 * ===== Controller Enable/Disable =====
 *
 * The controller must be enabled before any I/O operations can occur.
 * The enable sequence programs queue entry sizes and memory page size.
 */

/**
 * accel_enable_ctrl - Enable the controller
 * @dev: Device structure
 *
 * Sets CC.EN (Controller Configuration Enable) and waits for CSTS.RDY.
 *
 * Controller Configuration Register (CC) at offset 0x14:
 *   Bit 0:      EN - Enable controller when set
 *   Bits [6:4]: IOSQES - I/O SQ Entry Size (2^n bytes), we use 6 (64 bytes)
 *   Bits [10:8]: IOCQES - I/O CQ Entry Size (2^n bytes), we use 4 (16 bytes)
 *   Bits [14:12]: MPS - Memory Page Size (2^(12+n)), 0 = 4KB
 *
 * Returns: 0 on success, negative error code on failure
 */
int accel_enable_ctrl(struct accel_dev *dev)
{
	u32 cc;
	u32 timeout_ms = 5000;  /* 5 seconds */
	int ret;

	/*
	 * Set controller configuration:
	 * CC.EN = 1     (enable)
	 * CC.IOSQES = 6 (64 bytes per SQE, matches ACCEL_SQES)
	 * CC.IOCQES = 4 (16 bytes per CQE, matches ACCEL_CQES)
	 * CC.MPS = 0    (4KB memory page size)
	 */
	cc = (1 << 0) |        /* EN */
	     (ACCEL_SQES << 4) |  /* IOSQES */
	     (ACCEL_CQES << 8) |  /* IOCQES */
	     (0 << 12);        /* MPS (4KB) */

	accel_reg_write32(dev, ACCEL_REG_CC, cc);

	/* Wait for controller to become ready */
	ret = accel_wait_ready(dev, timeout_ms);
	if (ret) {
		dev_err(&dev->pdev->dev, "Controller failed to become ready\n");
		return ret;
	}

	dev_info(&dev->pdev->dev, "Controller enabled\n");
	return 0;
}

/**
 * accel_disable_ctrl - Disable the controller
 * @dev: Device structure
 *
 * Clears CC.EN and waits for CSTS.RDY to clear.
 * This gracefully shuts down command processing.
 */
void accel_disable_ctrl(struct accel_dev *dev)
{
	u32 cc;

	/* Clear enable bit while preserving other settings */
	cc = accel_reg_read32(dev, ACCEL_REG_CC);
	cc &= ~(1 << 0);  /* Clear EN */
	accel_reg_write32(dev, ACCEL_REG_CC, cc);

	/* Wait for controller to acknowledge disable */
	accel_wait_not_ready(dev, 5000);

	dev_info(&dev->pdev->dev, "Controller disabled\n");
}

/*
 * ===== PCI Driver Callbacks =====
 */

/**
 * accel_pci_probe - PCI device probe callback
 * @pdev: PCI device
 * @id: PCI device ID table entry
 *
 * Called when a matching PCI device is found. Performs:
 * 1. PCI device enable and bus master setup
 * 2. DMA mask configuration (64-bit preferred)
 * 3. BAR0 memory mapping
 * 4. MSI-X interrupt setup
 * 5. Request cache creation for io_uring async ops
 * 6. Admin queue initialization
 * 7. Controller enable
 * 8. Character device creation
 *
 * Returns: 0 on success, negative error code on failure
 */
static int accel_pci_probe(struct pci_dev *pdev,
			   const struct pci_device_id *id)
{
	struct accel_dev *dev;
	int ret;
	int dev_id;

	dev_info(&pdev->dev, "Probing PCIe Accelerator device\n");

	/* Allocate and initialize device structure */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pdev = pdev;
	mutex_init(&dev->dev_mutex);
	spin_lock_init(&dev->p2p_lock);
	INIT_LIST_HEAD(&dev->p2p_peers);

	/* Initialize statistics counters */
	atomic64_set(&dev->cmd_submitted, 0);
	atomic64_set(&dev->cmd_completed, 0);
	atomic64_set(&dev->p2p_transfers, 0);
	atomic64_set(&dev->uring_submissions, 0);
	atomic64_set(&dev->uring_completions, 0);

	pci_set_drvdata(pdev, dev);

	/* Enable PCI device */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PCI device: %d\n", ret);
		goto err_free_dev;
	}

	/* Request MMIO regions */
	ret = pci_request_regions(pdev, ACCEL_DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request PCI regions: %d\n", ret);
		goto err_disable_device;
	}

	/* Set PCI bus master for DMA operations */
	pci_set_master(pdev);

	/*
	 * Set DMA mask for 64-bit addressing.
	 * Fall back to 32-bit if 64-bit not supported (older systems).
	 */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "Failed to set DMA mask: %d\n", ret);
			goto err_release_regions;
		}
		dev_warn(&pdev->dev, "Using 32-bit DMA addressing\n");
	}

	/* Map BAR0 (controller registers) */
	dev->bar0 = pci_iomap(pdev, 0, 0);
	if (!dev->bar0) {
		dev_err(&pdev->dev, "Failed to map BAR0\n");
		ret = -ENOMEM;
		goto err_release_regions;
	}

	/* Log device capabilities */
	{
		u64 cap = accel_reg_read64(dev, ACCEL_REG_CAP);
		dev_info(&pdev->dev, "CAP=0x%016llx\n", cap);
	}

	/* Set up MSI-X interrupts for per-queue completion notification */
	ret = accel_setup_msix(dev);
	if (ret) {
		dev_warn(&pdev->dev, "MSI-X setup failed, using legacy IRQ\n");
		/* Continue without MSI-X - reduced performance but functional */
	}

	/* Allocate device ID first (needed for cache naming) */
	dev_id = ida_simple_get(&accel_ida, 0, ACCEL_MAX_DEVICES, GFP_KERNEL);
	if (dev_id < 0) {
		dev_err(&pdev->dev, "Failed to allocate device ID\n");
		ret = dev_id;
		goto err_free_msix;
	}
	dev->devt = MKDEV(MAJOR(accel_devt), dev_id);

	/* Create request slab cache for io_uring async operations */
	ret = accel_create_req_cache(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create request cache: %d\n", ret);
		goto err_free_ida;
	}

	/* Initialize admin queue */
	ret = accel_init_admin_queue(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to init admin queue: %d\n", ret);
		goto err_destroy_cache;
	}

	/* Enable controller - must succeed before any I/O */
	ret = accel_enable_ctrl(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable controller: %d\n", ret);
		goto err_free_admin;
	}

	/* Set up character device with io_uring support */
	ret = accel_setup_chardev(dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to setup chardev: %d\n", ret);
		goto err_disable_ctrl;
	}

	dev_info(&pdev->dev,
		 "PCIe Accelerator device initialized with io_uring support\n");
	return 0;

err_disable_ctrl:
	accel_disable_ctrl(dev);
err_free_admin:
	accel_free_admin_queue(dev);
err_destroy_cache:
	accel_destroy_req_cache(dev);
err_free_ida:
	ida_simple_remove(&accel_ida, dev_id);
err_free_msix:
	accel_free_msix(dev);
	pci_iounmap(pdev, dev->bar0);
err_release_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);
err_free_dev:
	kfree(dev);
	return ret;
}

/**
 * accel_pci_remove - PCI device remove callback
 * @pdev: PCI device
 *
 * Called when the device is being removed. Cleans up all resources
 * in reverse order of allocation to ensure proper shutdown.
 */
static void accel_pci_remove(struct pci_dev *pdev)
{
	struct accel_dev *dev = pci_get_drvdata(pdev);
	int i;

	dev_info(&pdev->dev, "Removing PCIe Accelerator device\n");

	/* Clean up character device first to stop new user operations */
	accel_cleanup_chardev(dev);

	/* Free device ID */
	ida_simple_remove(&accel_ida, MINOR(dev->devt));

	/* Clean up P2P peers while controller is still enabled */
	accel_cleanup_p2p_peers(dev);

	/* Disable controller - stops all command processing */
	accel_disable_ctrl(dev);

	/* Free all I/O queues (skip admin queue at 0) */
	for (i = 1; i < ACCEL_MAX_QUEUES; i++) {
		if (dev->queues[i])
			accel_delete_queue(dev, i);
	}

	/* Free admin queue */
	accel_free_admin_queue(dev);

	/* Destroy request cache */
	accel_destroy_req_cache(dev);

	/* Free MSI-X vectors */
	accel_free_msix(dev);

	/* Unmap BARs */
	if (dev->bar0)
		pci_iounmap(pdev, dev->bar0);

	/* Release PCI resources */
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	kfree(dev);
}

/**
 * accel_pci_shutdown - PCI shutdown callback
 * @pdev: PCI device
 *
 * Called during system shutdown. Disables the controller gracefully
 * to ensure no pending DMA operations are left in progress.
 */
static void accel_pci_shutdown(struct pci_dev *pdev)
{
	struct accel_dev *dev = pci_get_drvdata(pdev);

	if (dev)
		accel_disable_ctrl(dev);
}

static struct pci_driver accel_pci_driver = {
	.name		= ACCEL_DRIVER_NAME,
	.id_table	= accel_pci_tbl,
	.probe		= accel_pci_probe,
	.remove		= accel_pci_remove,
	.shutdown	= accel_pci_shutdown,
};

/*
 * ===== Module Init/Exit =====
 */

/**
 * accel_init - Module initialization
 *
 * Registers the PCI driver and creates the character device class.
 * The io_uring integration is handled per-device during probe.
 */
static int __init accel_init(void)
{
	int ret;

	pr_info("PCIe Accelerator driver version %s (io_uring enabled)\n",
		ACCEL_DRIVER_VERSION);

	/* Initialize queue subsystem (workqueue for completions) */
	ret = accel_queue_init();
	if (ret) {
		pr_err("Failed to initialize queue subsystem: %d\n", ret);
		return ret;
	}

	/* Allocate character device major number range */
	ret = alloc_chrdev_region(&accel_devt, 0, ACCEL_MAX_DEVICES,
				  ACCEL_DRIVER_NAME);
	if (ret < 0) {
		pr_err("Failed to allocate chrdev region: %d\n", ret);
		goto err_queue_exit;
	}

	/* Create device class for udev */
	accel_class = class_create(ACCEL_DRIVER_NAME);
	if (IS_ERR(accel_class)) {
		ret = PTR_ERR(accel_class);
		pr_err("Failed to create device class: %d\n", ret);
		goto err_unregister_chrdev;
	}

	/* Register PCI driver */
	ret = pci_register_driver(&accel_pci_driver);
	if (ret) {
		pr_err("Failed to register PCI driver: %d\n", ret);
		goto err_destroy_class;
	}

	return 0;

err_destroy_class:
	class_destroy(accel_class);
err_unregister_chrdev:
	unregister_chrdev_region(accel_devt, ACCEL_MAX_DEVICES);
err_queue_exit:
	accel_queue_exit();
	return ret;
}

/**
 * accel_exit - Module cleanup
 *
 * Unregisters the PCI driver and cleans up global resources.
 */
static void __exit accel_exit(void)
{
	pci_unregister_driver(&accel_pci_driver);
	class_destroy(accel_class);
	unregister_chrdev_region(accel_devt, ACCEL_MAX_DEVICES);
	ida_destroy(&accel_ida);
	accel_queue_exit();

	pr_info("PCIe Accelerator driver unloaded\n");
}

module_init(accel_init);
module_exit(accel_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("QEMU Project");
MODULE_DESCRIPTION("PCIe Accelerator Device Driver with io_uring Support");
MODULE_VERSION(ACCEL_DRIVER_VERSION);

/* Export class for chardev module */
struct class *accel_get_class(void)
{
	return accel_class;
}
EXPORT_SYMBOL_GPL(accel_get_class);

/* Export request pool size parameter for queue module */
unsigned int accel_get_req_pool_size(void)
{
	return req_pool_size;
}
EXPORT_SYMBOL_GPL(accel_get_req_pool_size);
