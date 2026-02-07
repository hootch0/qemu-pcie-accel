/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * PCIe Accelerator Device - Linux Kernel Driver Header
 *
 * Copyright (C) 2026
 *
 * This driver uses io_uring for high-performance asynchronous I/O.
 * Key features:
 * - io_uring command passthrough for low-latency submissions
 * - Async completion via io_uring CQEs
 * - Support for batched submissions
 * - Direct doorbell access via mmap
 */

#ifndef _PCIE_ACCEL_H
#define _PCIE_ACCEL_H

#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/iommu.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/hashtable.h>
#include <linux/io_uring.h>
#include <linux/io_uring/cmd.h>

/* Driver information */
#define ACCEL_DRIVER_NAME	"pcie-accel"
#define ACCEL_DRIVER_VERSION	"2.0.0"

/* Device limits */
#define ACCEL_MAX_DEVICES	16
#define ACCEL_MAX_QUEUES	256
#define ACCEL_MAX_P2P_PEERS	32
#define ACCEL_MAX_INFLIGHT	4096

/* Register offsets (must match QEMU device) */
#define ACCEL_REG_CAP		0x0000
#define ACCEL_REG_VS		0x0008
#define ACCEL_REG_INTMS		0x000C
#define ACCEL_REG_INTMC		0x0010
#define ACCEL_REG_CC		0x0014
#define ACCEL_REG_CSTS		0x001C
#define ACCEL_REG_AQA		0x0024
#define ACCEL_REG_ASQ		0x0028
#define ACCEL_REG_ACQ		0x0030
#define ACCEL_REG_P2PCFG	0x0040
#define ACCEL_REG_INTCOAL	0x0050
#define ACCEL_REG_DEVSTAT	0x0058
#define ACCEL_REG_P2QQCFG	0x0060
#define ACCEL_REG_DOORBELL	0x1000
#define ACCEL_P2Q_DB_BASE	0x4000
#define ACCEL_P2Q_DB_STRIDE	8

/* Command entry and completion entry sizes */
#define ACCEL_SQES		6	/* 2^6 = 64 bytes */
#define ACCEL_CQES		4	/* 2^4 = 16 bytes */
#define ACCEL_SQE_SIZE		(1 << ACCEL_SQES)
#define ACCEL_CQE_SIZE		(1 << ACCEL_CQES)

/* Command opcodes */
#define ACCEL_ADM_CMD_DELETE_SQ		0x00
#define ACCEL_ADM_CMD_CREATE_SQ		0x01
#define ACCEL_ADM_CMD_DELETE_CQ		0x04
#define ACCEL_ADM_CMD_CREATE_CQ		0x05
#define ACCEL_ADM_CMD_IDENTIFY		0x06
#define ACCEL_ADM_CMD_P2P_SETUP		0x10
#define ACCEL_ADM_CMD_P2P_QUEUE_SETUP	0x12
#define ACCEL_ADM_CMD_P2P_QUEUE_TEARDOWN 0x13

#define ACCEL_CMD_LOOPBACK		0x01
#define ACCEL_CMD_P2P_WRITE		0x02
#define ACCEL_CMD_P2P_READ		0x03

/* P2P MMIO Queue command opcodes (device-to-device) */
#define ACCEL_P2Q_CMD_MMIO_WRITE	0x80
#define ACCEL_P2Q_CMD_MMIO_READ		0x81
#define ACCEL_P2Q_CMD_LOOPBACK		0x82

/* P2P Queue BAR2 layout */
#define ACCEL_P2Q_MAX_SLOTS		7
#define ACCEL_P2Q_SQ_ENTRIES		64
#define ACCEL_P2Q_CQ_ENTRIES		64
#define ACCEL_P2Q_SQ_OFFSET(slot)	((slot) * 0x1000)
#define ACCEL_P2Q_CQ_BASE		0x8000
#define ACCEL_P2Q_CQ_OFFSET(slot)	(ACCEL_P2Q_CQ_BASE + (slot) * 0x400)
#define ACCEL_P2Q_DATA_OFFSET		0xC000

/* Status codes */
#define ACCEL_SC_SUCCESS		0x00
#define ACCEL_SC_INVALID_OPCODE		0x01
#define ACCEL_SC_INVALID_FIELD		0x02
#define ACCEL_SC_DMA_ERROR		0x30

/* io_uring command opcodes for the driver */
enum accel_uring_cmd_op {
	ACCEL_URING_CMD_SUBMIT = 0,	/* Submit device command */
	ACCEL_URING_CMD_CREATE_QUEUE,	/* Create queue pair */
	ACCEL_URING_CMD_DELETE_QUEUE,	/* Delete queue pair */
	ACCEL_URING_CMD_SETUP_P2P,	/* Setup P2P peer */
	ACCEL_URING_CMD_GET_STATS,	/* Get statistics */
	ACCEL_URING_CMD_ADMIN,		/* Admin command */
	ACCEL_URING_CMD_P2P_QUEUE_SETUP, /* Setup P2P MMIO queue */
};

/* Forward declarations */
struct accel_dev;
struct accel_queue;
struct accel_request;

/**
 * struct accel_cmd - Submission queue entry (64 bytes)
 */
struct accel_cmd {
	__u8	opcode;
	__u8	flags;
	__le16	cid;
	__le32	nsid;
	__le64	rsvd1;
	__le64	metadata;
	__le64	prp1;
	__le64	prp2;
	union {
		struct {
			__le32	length;
			__le32	rsvd;
			__le64	peer_addr;
			__le32	peer_bdf;
			__le32	pasid;
		} p2p;
		struct {
			__le32	length;
			__le32	pattern;
			__le32	flags;
			__le32	rsvd[3];
		} loopback;
		struct {
			__le32	cdw10;
			__le32	cdw11;
			__le32	cdw12;
			__le32	cdw13;
			__le32	cdw14;
			__le32	cdw15;
		} admin;
	} dw;
} __packed;

/**
 * struct accel_cqe - Completion queue entry (16 bytes)
 */
struct accel_cqe {
	__le32	result;
	__le32	rsvd;
	__le16	sq_head;
	__le16	sq_id;
	__le16	cid;
	__le16	status;
} __packed;

/**
 * struct accel_uring_cmd - io_uring command structure
 *
 * This is passed via io_uring SQE's cmd field (80 bytes available).
 * Layout designed for cache-friendly access.
 */
struct accel_uring_cmd {
	__u8	op;		/* accel_uring_cmd_op */
	__u8	flags;		/* Command flags */
	__u16	qid;		/* Queue ID */
	__u32	timeout_ms;	/* Timeout in milliseconds */

	union {
		/* For ACCEL_URING_CMD_SUBMIT */
		struct {
			struct accel_cmd cmd;	/* Device command */
		} submit;

		/* For ACCEL_URING_CMD_CREATE_QUEUE */
		struct {
			__u16	sq_size;
			__u16	cq_size;
			__u16	flags;
			__u16	rsvd;
		} create_queue;

		/* For ACCEL_URING_CMD_DELETE_QUEUE */
		struct {
			__u16	qid;
			__u16	rsvd[3];
		} delete_queue;

		/* For ACCEL_URING_CMD_SETUP_P2P */
		struct {
			__u16	peer_bdf;
			__u16	flags;
			__u32	rsvd;
		} setup_p2p;
	};
} __packed;

/**
 * struct accel_uring_result - io_uring completion result
 *
 * Returned in io_uring CQE's extra fields or via big_cqe.
 */
struct accel_uring_result {
	__s32	status;		/* Command status (0 = success) */
	__u32	result;		/* Command-specific result */
	__u16	sq_head;	/* SQ head at completion */
	__u16	sq_id;		/* SQ ID */
	__u16	cid;		/* Command ID */
	__u16	dev_status;	/* Device status code */
};

/**
 * struct accel_request - In-flight request tracking
 *
 * Tracks async requests through their lifecycle.
 * Allocated from a slab cache for efficiency.
 */
struct accel_request {
	struct accel_queue *queue;		/* Owning queue */
	struct io_uring_cmd *ioucmd;		/* io_uring command */

	u16 cid;				/* Command ID */
	u16 status;				/* Completion status */

	struct accel_cmd cmd;			/* Copy of command */
	struct accel_cqe cqe;			/* Completion entry */

	/* DMA resources */
	void *data_buf;				/* DMA buffer */
	dma_addr_t data_dma;			/* DMA address */
	size_t data_len;			/* Buffer length */

	/* User buffer for read operations (copy back on completion) */
	void __user *user_buf;			/* Original user address */
	bool is_read;				/* True if read operation */

	/* For request tracking */
	struct hlist_node hash_node;		/* Hash table linkage */
	struct list_head list;			/* Free/pending list */

	/* Timing for timeout handling */
	unsigned long start_time;		/* jiffies at submit */
	struct timer_list timer;		/* Timeout timer */

	/* Deferred completion for read operations */
	struct work_struct completion_work;	/* Work for copy_to_user */
	s32 result;				/* Device result for deferred completion */
	struct mm_struct *mm;			/* User's mm for kthread_use_mm */
};

/**
 * struct accel_queue - Queue pair structure
 */
struct accel_queue {
	struct accel_dev *dev;
	u16 qid;
	u16 sq_size;
	u16 cq_size;

	void *sq_buffer;
	void *cq_buffer;
	dma_addr_t sq_dma_addr;
	dma_addr_t cq_dma_addr;

	u32 sq_head;
	u32 sq_tail;
	u32 cq_head;
	u8 cq_phase;

	spinlock_t sq_lock;
	spinlock_t cq_lock;

	int irq_vector;

	/* Request tracking */
	DECLARE_HASHTABLE(req_hash, 8);		/* CID -> request lookup */
	struct list_head req_free;		/* Free request pool */
	struct list_head req_pending;		/* In-flight requests */
	struct accel_request *req_pool;		/* Pre-allocated requests */
	u32 req_pool_size;

	/* Statistics */
	atomic64_t submitted;
	atomic64_t completed;
};

/**
 * struct accel_p2p_peer - P2P peer device information
 */
struct accel_p2p_peer {
	struct pci_dev *pdev;
	u16 bdf;
	void __iomem *mem;		/* BAR2 scratchpad mapping */
	resource_size_t mem_size;	/* BAR2 size */
	resource_size_t mem_phys;	/* BAR2 physical address */
	struct list_head list;
};

/**
 * struct accel_dev - Main device structure
 */
/**
 * struct accel_p2p_queue_setup - P2P MMIO queue setup parameters
 */
struct accel_p2p_queue_setup {
	__u16	peer_bdf;		/* Peer device BDF */
	__u8	slot;			/* Slot in our device (0-6) */
	__u8	peer_slot;		/* Our slot in peer's device (0-6) */
	__u64	peer_bar0;		/* Peer's BAR0 physical address */
	__u64	peer_bar2;		/* Peer's BAR2 physical address */
};

struct accel_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;			/* BAR0: Controller registers */
	void __iomem *bar2;			/* BAR2: P2P queues + data */
	resource_size_t bar2_size;		/* BAR2 size */

	struct cdev cdev;
	dev_t devt;
	struct device *char_dev;
	struct mutex dev_mutex;

	struct accel_queue *queues[ACCEL_MAX_QUEUES];
	u32 num_queues;

	int num_vecs;
	struct msix_entry *msix_entries;

	struct list_head p2p_peers;
	spinlock_t p2p_lock;

	struct iommu_domain *domain;
	bool pasid_enabled;
	u32 pasid;

	/* io_uring support */
	struct kmem_cache *req_cache;		/* Request slab cache */

	/* Statistics */
	atomic64_t cmd_submitted;
	atomic64_t cmd_completed;
	atomic64_t p2p_transfers;
	atomic64_t uring_submissions;
	atomic64_t uring_completions;
};

/* IOCTL commands (for compatibility, prefer io_uring) */
#define ACCEL_IOC_MAGIC 'A'
#define ACCEL_IOC_CREATE_QUEUE	_IOWR(ACCEL_IOC_MAGIC, 1, struct accel_uring_cmd)
#define ACCEL_IOC_DELETE_QUEUE	_IOW(ACCEL_IOC_MAGIC, 2, __u16)
#define ACCEL_IOC_SUBMIT_CMD	_IOWR(ACCEL_IOC_MAGIC, 3, struct accel_uring_cmd)
#define ACCEL_IOC_SETUP_P2P	_IOW(ACCEL_IOC_MAGIC, 4, struct accel_uring_cmd)
#define ACCEL_IOC_GET_STATS	_IOR(ACCEL_IOC_MAGIC, 5, struct accel_uring_result)
#define ACCEL_IOC_P2P_QUEUE_SETUP _IOW(ACCEL_IOC_MAGIC, 6, struct accel_p2p_queue_setup)

/* Helper functions */
static inline u32 accel_reg_read32(struct accel_dev *dev, u32 offset)
{
	return readl(dev->bar0 + offset);
}

static inline void accel_reg_write32(struct accel_dev *dev, u32 offset, u32 val)
{
	writel(val, dev->bar0 + offset);
}

static inline u64 accel_reg_read64(struct accel_dev *dev, u32 offset)
{
	return readq(dev->bar0 + offset);
}

static inline void accel_reg_write64(struct accel_dev *dev, u32 offset, u64 val)
{
	writeq(val, dev->bar0 + offset);
}

/* Function declarations */

/* Core driver (pcie-accel-core.c) */
int accel_setup_msix(struct accel_dev *dev);
void accel_free_msix(struct accel_dev *dev);
int accel_init_admin_queue(struct accel_dev *dev);
void accel_free_admin_queue(struct accel_dev *dev);
int accel_enable_ctrl(struct accel_dev *dev);
void accel_disable_ctrl(struct accel_dev *dev);
struct class *accel_get_class(void);
unsigned int accel_get_req_pool_size(void);

/* Queue management (pcie-accel-queue.c) */
int accel_queue_init(void);
void accel_queue_exit(void);
int accel_create_queue(struct accel_dev *dev, u16 qid, u16 sq_size, u16 cq_size);
int accel_delete_queue(struct accel_dev *dev, u16 qid);
int accel_submit_sync_cmd(struct accel_dev *dev, u16 qid,
			  struct accel_cmd *cmd, struct accel_cqe *cqe,
			  u32 timeout_ms);
int accel_submit_admin_cmd(struct accel_dev *dev, struct accel_cmd *cmd,
			   struct accel_cqe *cqe);
int accel_submit_async_cmd(struct accel_queue *queue, struct accel_cmd *cmd,
			   struct io_uring_cmd *ioucmd, void *data_buf,
			   dma_addr_t data_dma, size_t data_len,
			   void __user *user_buf);
void accel_complete_request(struct accel_request *req);
int accel_ring_sq_doorbell(struct accel_queue *queue);
int accel_ring_cq_doorbell(struct accel_queue *queue);

/* Interrupt handling */
irqreturn_t accel_irq_handler(int irq, void *data);
irqreturn_t accel_irq_handler_threaded(int irq, void *data);

/* Character device (pcie-accel-chardev.c) */
int accel_setup_chardev(struct accel_dev *dev);
void accel_cleanup_chardev(struct accel_dev *dev);

/* io_uring command handler */
int accel_uring_cmd(struct io_uring_cmd *ioucmd, unsigned int issue_flags);

/* P2P support (pcie-accel-p2p.c) */
int accel_setup_p2p_peer(struct accel_dev *dev, u16 peer_bdf);
void accel_cleanup_p2p_peers(struct accel_dev *dev);
int accel_enable_pasid(struct accel_dev *dev);
void accel_disable_pasid(struct accel_dev *dev);
int accel_p2p_dma_map(struct accel_dev *dev, struct accel_p2p_peer *peer,
		      void *addr, size_t len, dma_addr_t *dma_addr);
void accel_p2p_dma_unmap(struct accel_dev *dev, dma_addr_t dma_addr, size_t len);
int accel_p2p_queue_setup(struct accel_dev *dev,
			  struct accel_p2p_queue_setup *params);

/* Request management */
struct accel_request *accel_alloc_request(struct accel_queue *queue);
void accel_free_request(struct accel_request *req);

#endif /* _PCIE_ACCEL_H */
