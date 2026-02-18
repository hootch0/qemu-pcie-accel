/*
 * QEMU PCIe Accelerator Device - Device Structures
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This header defines the core data structures for the PCIe Accelerator device.
 * The device implements submission/completion queues with doorbell registers,
 * supports P2P DMA between multiple devices, PASID/SVA for shared virtual
 * addressing, and MSI-X interrupts with coalescing.
 */

#ifndef HW_PCIE_ACCELERATOR_H
#define HW_PCIE_ACCELERATOR_H

#include "hw/pci/pci_device.h"
#include "hw/pci/pcie.h"
#include "qemu/queue.h"
#include "qom/object.h"
#include "system/hostmem.h"
#include "system/dma.h"
#include "hw/misc/pcie-accelerator-regs.h"

#define TYPE_PCIE_ACCEL "pcie-accelerator"
OBJECT_DECLARE_SIMPLE_TYPE(PCIeAccel, PCIE_ACCEL)

/*
 * ===== Command and Completion Entry Structures =====
 *
 * These structures define the format of submission and completion queue entries.
 * They are shared between the device and driver, so layout must be exact.
 */

/*
 * Submission Queue Entry (64 bytes)
 *
 * Generic command structure using Command Dword (CDW) layout.
 * Commands are 64 bytes to match cache line size for efficient DMA.
 */
typedef union QEMU_PACKED AccelCmd {
    /* Generic command format */
    struct QEMU_PACKED {
        /* CDW0 */
        uint8_t  opcode;          /* Command opcode (see ACCEL_CMD_* in regs.h) */
        uint8_t  flags;           /* [1:0]=DBD type, [2]=PASID, [3]=PRIV */
        uint16_t cid;             /* Command identifier (unique within SQ) */

        /* CDW1-2: Reserved */
        uint32_t rsvd0;           /* Reserved (was nsid) */
        uint32_t rsvd1;           /* Reserved */

        /* CDW3-6: Data Block Descriptor (16 bytes) */
        union {
            struct { uint64_t prp1; uint64_t prp2; } prpl;
            struct { uint64_t addr; uint32_t length; uint32_t type; } sgl;
            struct { uint64_t addr; uint64_t rsvd; } hva;
        } dbd;

        /* CDW7: Data transfer size */
        uint32_t data_xfer_size;

        /* CDW8-9: Reserved */
        uint64_t rsvd2;

        /* CDW10-15: Command-specific parameters */
        union {
            /* P2P transfer parameters (for P2P_WRITE/P2P_READ commands) */
            struct {
                uint32_t length;       /* Transfer length in bytes */
                uint32_t rsvd;
                uint64_t peer_addr;    /* Peer device physical address (DPA) */
                uint32_t peer_bdf;     /* Peer device Bus:Device:Function */
                uint32_t pasid;        /* Process Address Space ID (if PASID enabled) */
            } p2p;

            /* Loopback test parameters */
            struct {
                uint32_t length;       /* Buffer length in bytes */
                uint32_t pattern;      /* Data pattern for verification (optional) */
                uint32_t flags;        /* Loopback-specific flags */
                uint32_t rsvd[3];
            } loopback;

            /* Admin command parameters */
            struct {
                uint32_t cdw10;
                uint32_t cdw11;
                uint32_t cdw12;
                uint32_t cdw13;
                uint32_t cdw14;
                uint32_t cdw15;
            } admin;
        } dw;
    };

    /* Create IO Queue command (opcode 0x0D) */
    struct QEMU_PACKED {
        uint8_t  opcode;
        uint8_t  flags;
        uint16_t cid;
        uint16_t qid;             /* Queue pair ID */
        uint16_t irq_vector;      /* MSI-X interrupt vector */
        uint64_t sq_base;         /* SQ DMA base address */
        uint64_t cq_base;         /* CQ DMA base address */
        uint32_t reserved[10];
    } create_ioq;

    /* Delete IO Queue command (opcode 0x0E) */
    struct QEMU_PACKED {
        uint8_t  opcode;
        uint8_t  flags;
        uint16_t cid;
        uint16_t qid;             /* Queue pair ID to delete */
        uint16_t reserved;
        uint32_t reserved2[14];
    } delete_ioq;

    /* Memory Read command (opcode 0x05) */
    struct QEMU_PACKED {
        uint8_t  opcode;
        uint8_t  flags;
        uint16_t cid;
        uint64_t dev_addr;         /* Device memory address (CMB offset) */
        uint64_t host_addr;        /* Host buffer address (PRP1) */
        uint64_t reserved0;
        uint32_t length;           /* Transfer length in bytes */
        uint32_t reserved[8];
    } mem_read;

    /* Memory Write command (opcode 0x06) */
    struct QEMU_PACKED {
        uint8_t  opcode;
        uint8_t  flags;
        uint16_t cid;
        uint64_t dev_addr;         /* Device memory address (CMB offset) */
        uint64_t host_addr;        /* Host buffer address (PRP1) */
        uint64_t reserved0;
        uint32_t length;           /* Transfer length in bytes */
        uint32_t reserved[8];
    } mem_write;

    /* P2P Setup command (opcode 0x0F) */
    struct QEMU_PACKED {
        uint8_t  opcode;
        uint8_t  flags;
        uint16_t cid;
        uint16_t peer_bdf;        /* Peer Bus:Device:Function */
        uint8_t  slot;            /* Ring slot in our device (0-6) */
        uint8_t  peer_slot;       /* Our slot in peer's device (0-6) */
        uint64_t peer_bar0;       /* Peer's BAR0 physical address */
        uint32_t reserved[12];
    } p2p_setup;

    /* P2P Teardown command (opcode 0x10) */
    struct QEMU_PACKED {
        uint8_t  opcode;
        uint8_t  flags;
        uint16_t cid;
        uint16_t peer_bdf;        /* Peer BDF to unregister */
        uint8_t  slot;            /* Ring slot to tear down (0-6) */
        uint8_t  reserved;
        uint32_t reserved2[14];
    } p2p_teardown;
} AccelCmd;

QEMU_BUILD_BUG_ON(sizeof(AccelCmd) != 64);

/*
 * Completion Queue Entry (16 bytes)
 *
 * Minimal completion entry with phase bit for wrap-around detection.
 * The phase bit toggles each time the CQ wraps to the beginning.
 */
typedef struct QEMU_PACKED AccelCqe {
    uint16_t sq_head;         /* SQ head pointer at completion time */
    uint16_t cid;             /* Command identifier from submission */
    uint32_t status;          /* SC[15:0], SCT[23:16], rsvd[30:24], Phase[31] */
    uint64_t result;          /* Command-specific result (64-bit) */
} AccelCqe;

QEMU_BUILD_BUG_ON(sizeof(AccelCqe) != 16);

/*
 * Identify Data Structures (4096 bytes total)
 *
 * Returned by the Identify admin command (opcode 0x00).
 */

/* Hardware Info (1020 bytes) */
typedef struct QEMU_PACKED AccelDevHwInfo {
    uint16_t tid;                   /* Type ID */
    uint16_t dev_id;                /* Device ID */
    uint8_t  reserved[1016];
} AccelDevHwInfo;

QEMU_BUILD_BUG_ON(sizeof(AccelDevHwInfo) != 1020);

/* Memory Region Descriptor (16 bytes) */
typedef struct QEMU_PACKED AccelDevMemRegion {
    uint64_t desc;                  /* cid[5:0], type[7:6], pid[15:8], size[63:16] */
    uint64_t addr;                  /* Region base address */
} AccelDevMemRegion;

QEMU_BUILD_BUG_ON(sizeof(AccelDevMemRegion) != 16);

/* Identify Data (4096 bytes) */
typedef struct QEMU_PACKED AccelIdData {
    uint32_t        data_len;       /* Total data length in bytes */
    AccelDevHwInfo  hw_info;        /* Hardware info */
    uint32_t        mem_region_count; /* Number of valid memory regions */
    uint8_t         rsvd[12];       /* Reserved */
    AccelDevMemRegion mem_regions[ACCEL_ID_MAX_MEM_REGIONS]; /* Memory regions */
} AccelIdData;

QEMU_BUILD_BUG_ON(sizeof(AccelIdData) != 4096);

/*
 * CQE status field layout (32 bits):
 *   [15:0]  - Status Code (SC)
 *   [23:16] - Status Code Type (SCT)
 *   [30:24] - Reserved
 *   [31]    - Phase (P) - toggles on CQ wrap
 */
#define ACCEL_CQE_STATUS_SC_MASK      0x0000FFFF
#define ACCEL_CQE_STATUS_SCT_SHIFT    16
#define ACCEL_CQE_STATUS_SCT_MASK     0x00FF0000
#define ACCEL_CQE_STATUS_PHASE_SHIFT  31
#define ACCEL_CQE_STATUS_PHASE_MASK   0x80000000

/* Extract fields from CQE status */
#define ACCEL_CQE_SC(status)    ((status) & ACCEL_CQE_STATUS_SC_MASK)
#define ACCEL_CQE_SCT(status)   (((status) >> ACCEL_CQE_STATUS_SCT_SHIFT) & 0xFF)
#define ACCEL_CQE_PHASE(status) (((status) >> ACCEL_CQE_STATUS_PHASE_SHIFT) & 0x1)

/* Build CQE status from status code, status code type, and phase */
#define ACCEL_CQE_BUILD_STATUS(sc, sct, phase) \
    (((sc) & ACCEL_CQE_STATUS_SC_MASK) | \
     (((sct) << ACCEL_CQE_STATUS_SCT_SHIFT) & ACCEL_CQE_STATUS_SCT_MASK) | \
     (((phase) << ACCEL_CQE_STATUS_PHASE_SHIFT) & ACCEL_CQE_STATUS_PHASE_MASK))

/*
 * ===== Queue State Structures =====
 */

/* Forward declarations */
typedef struct PCIeAccel PCIeAccel;
typedef struct AccelSQueue AccelSQueue;
typedef struct AccelCQueue AccelCQueue;
typedef struct AccelRequest AccelRequest;
typedef struct AccelP2PPeer AccelP2PPeer;

/*
 * Request Tracking Structure
 *
 * Tracks in-flight commands through their lifecycle. Requests are pre-allocated
 * per submission queue to avoid dynamic allocation in the hot path.
 */
struct AccelRequest {
    AccelSQueue *sq;                    /* Owning submission queue */
    uint16_t status;                    /* Completion status code */

    AccelCqe cqe;                       /* Completion queue entry (built here) */
    AccelCmd cmd;                       /* Original command (copied from host) */

    QEMUSGList qsg;                     /* Scatter-gather list for DMA */

    QTAILQ_ENTRY(AccelRequest) entry;   /* Queue linkage */

    /* P2P transfer state */
    struct {
        bool is_p2p;                    /* True if this is a P2P command */
        uint16_t peer_bdf;              /* Peer device BDF */
        AddressSpace *peer_as;          /* Peer device address space */
        uint32_t pasid;                 /* PASID if SVA enabled */
    } p2p;

    /* DMA bounce buffer for chunked transfers */
    void *bounce_buf;
    size_t bounce_len;
};

/*
 * Submission Queue
 *
 * One submission queue per queue pair. Commands are fetched from host memory
 * when the doorbell is rung. Processing is deferred to a bottom-half for
 * async execution.
 */
struct AccelSQueue {
    PCIeAccel *ctrl;                    /* Parent controller */

    uint16_t sqid;                      /* Submission queue ID */
    uint16_t cqid;                      /* Associated completion queue ID */

    uint32_t head;                      /* SQ head pointer (device-owned) */
    uint32_t tail;                      /* SQ tail pointer (host-owned) */
    uint32_t size;                      /* Queue size in entries */

    uint64_t dma_addr;                  /* Queue base DMA address in host memory */

    QEMUBH *bh;                         /* Bottom-half for async processing */

    /* Pre-allocated request pool */
    AccelRequest *io_req;               /* Array of request structures */
    QTAILQ_HEAD(, AccelRequest) req_list;       /* Available requests */
    QTAILQ_HEAD(, AccelRequest) out_req_list;   /* In-flight requests */

    QTAILQ_ENTRY(AccelSQueue) entry;    /* Linkage for CQ's SQ list */
};

/*
 * Completion Queue
 *
 * One or more submission queues can be associated with a single completion queue.
 * Completions are posted asynchronously via bottom-half. The phase bit toggles
 * on wrap to allow lock-free completion detection by the driver.
 */
struct AccelCQueue {
    PCIeAccel *ctrl;                    /* Parent controller */

    uint8_t phase;                      /* Current phase bit (0 or 1) */
    uint16_t cqid;                      /* Completion queue ID */
    uint16_t irq_enabled;               /* Interrupt enabled flag */

    uint32_t head;                      /* CQ head pointer (host-owned) */
    uint32_t tail;                      /* CQ tail pointer (device-owned) */
    uint32_t size;                      /* Queue size in entries */
    uint32_t vector;                    /* MSI-X vector number */

    uint64_t dma_addr;                  /* Queue base DMA address in host memory */

    QEMUBH *bh;                         /* Bottom-half for completion posting */

    QTAILQ_HEAD(, AccelSQueue) sq_list;     /* Associated submission queues */
    QTAILQ_HEAD(, AccelRequest) req_list;   /* Requests ready for completion */
};

/*
 * P2P Peer Device Tracking
 *
 * Tracks registered peer devices for P2P DMA transfers. Each peer has its own
 * address space for DMA routing and a counter for active transfers to prevent
 * overload.
 */
struct AccelP2PPeer {
    uint16_t bdf;                       /* Bus:Device:Function (identifies peer) */
    PCIDevice *pci_dev;                 /* Peer PCI device pointer */
    AddressSpace *as;                   /* Peer's DMA address space */
    MemoryRegion *cmb;                  /* Peer's CMB memory region */

    bool enabled;                       /* Peer is enabled and ready */
    uint32_t active_xfers;              /* Current active transfers to this peer */

    QTAILQ_ENTRY(AccelP2PPeer) entry;   /* Linkage for peer list */
};

/*
 * P2P Ring Buffer Header (in CMB RAM)
 *
 * Each ring buffer starts with a 64-byte header followed by the data area.
 */
typedef struct QEMU_PACKED AccelRingHdr {
    uint32_t head;                      /* Consumer position (byte offset in data area) */
    uint32_t tail;                      /* Producer position (byte offset in data area) */
    uint32_t size;                      /* Data area capacity in bytes */
    uint32_t flags;                     /* Ring status flags */
    uint32_t reserved[12];              /* Pad to 64 bytes */
} AccelRingHdr;

QEMU_BUILD_BUG_ON(sizeof(AccelRingHdr) != 64);

/*
 * P2P Ring Message (variable length, 8-byte aligned)
 *
 * Messages are enqueued in the ring data area as a circular buffer.
 */
typedef struct QEMU_PACKED AccelRingMsg {
    uint16_t type;                      /* ACCEL_RING_MSG_DATA/NOTIFY/STATUS */
    uint16_t flags;                     /* Per-message flags */
    uint32_t length;                    /* Total length including header (8-byte aligned) */
} AccelRingMsg;

QEMU_BUILD_BUG_ON(sizeof(AccelRingMsg) != 8);

/*
 * P2P Ring Buffer
 *
 * Each slot represents a unidirectional inbound ring buffer. Peer devices
 * produce messages into our ring; we consume them. For the reverse direction,
 * we write to the peer's inbound ring.
 *
 * Ring data lives in BAR2 CMB (RAM). Doorbells are in BAR0 MMIO.
 * Cross-device communication uses address_space_write() to peer BAR0.
 */
typedef struct AccelP2PRing {
    struct PCIeAccel *ctrl;             /* Parent controller */
    uint8_t  slot;                      /* Slot index (0 to P2R_MAX_SLOTS-1) */
    uint16_t peer_bdf;                  /* Peer device BDF */
    bool     active;                    /* Slot is configured and active */

    /* Inbound ring state (mirrors ring header in CMB) */
    uint32_t head;                      /* Consumer position (we advance) */
    uint32_t tail;                      /* Producer position (peer updates via doorbell) */
    uint32_t size;                      /* Data area capacity in bytes */

    /* Outbound tracking (for writing to peer's ring) */
    struct {
        uint8_t  our_slot;              /* Our slot index in peer's device */
        hwaddr   peer_bar0;             /* Peer's BAR0 physical address */
        AddressSpace *peer_as;          /* Peer's PCI address space */
        PCIDevice *peer_dev;            /* Peer PCI device pointer */
    } outbound;

    QEMUBH *bh;                         /* BH for processing inbound ring messages */
} AccelP2PRing;

/*
 * ===== Main Device State =====
 *
 * The PCIeAccel structure represents the entire device state. It extends
 * PCIDevice and contains all queues, configuration, and runtime state.
 */
struct PCIeAccel {
    PCIDevice parent_obj;

    /* Memory Regions */
    MemoryRegion bar0;                  /* BAR0: MMIO registers (64KB) */
    MemoryRegion cmb;                   /* BAR2: CMB RAM */
    MemoryRegion msix_bar;              /* BAR4: MSI-X table/PBA (16KB) */

    /* Device Physical Address (DPA) Memory - internal, not BAR-mapped */
    HostMemoryBackend *dpa_memdev;      /* DPA file-backed memory backend */
    MemoryRegion *dpa_mr;               /* DPA memory region (from backend) */

    /* Device Registers (in-memory representation of BAR0) */
    struct {
        uint64_t cap;                   /* Capability register */
        uint32_t cc;                    /* Configuration */
        uint32_t csts;                  /* Status */
        uint64_t asq;                   /* Admin SQ base address */
        uint64_t acq;                   /* Admin CQ base address */
        uint32_t cmbbar;                /* CMB BAR number */
        uint32_t cmbsz;                 /* CMB size in bytes */
        uint32_t p2pcfg;                /* P2P configuration */
        uint32_t intcoal;               /* Interrupt coalescing */
        uint32_t devstat;               /* Device status */
    } bar;

    /* Queue Management */
    uint32_t conf_ioqpairs;             /* Configured I/O queue pairs */
    uint32_t max_ioqpairs;              /* Maximum I/O queue pairs (from property) */

    AccelSQueue **sq;                   /* Submission queue array [0..max_ioqpairs] */
    AccelCQueue **cq;                   /* Completion queue array [0..max_ioqpairs] */

    AccelSQueue admin_sq;               /* Admin submission queue (sqid=0) */
    AccelCQueue admin_cq;               /* Admin completion queue (cqid=0) */

    /* Interrupt Coalescing Parameters */
    uint8_t intcoal_thresh;             /* Completion threshold */
    uint8_t intcoal_time;               /* Time threshold in 100us units */

    /* P2P DMA State */
    struct {
        uint8_t max_peers;              /* Maximum peer devices (from property) */
        uint8_t max_xfers_per_peer;     /* Max concurrent xfers per peer */
        uint8_t num_peers;              /* Current number of registered peers */

        AccelP2PPeer peers[ACCEL_MAX_P2P_PEERS];  /* Peer device array */
        QTAILQ_HEAD(, AccelP2PPeer) peer_list;    /* Active peer list */

        /* P2P Ring Buffers */
        AccelP2PRing p2p_rings[ACCEL_P2R_MAX_SLOTS];
        uint8_t num_p2p_rings;          /* Number of active P2P ring buffers */
        uint32_t p2rcfg;                /* P2P Ring Configuration register */
    } p2p;

    /* PASID/SVA Support */
    struct {
        bool enabled;                   /* PASID support enabled */
        uint8_t pasid_width;            /* Number of PASID bits (8-20) */
        AddressSpace **pasid_as;        /* Per-PASID address spaces */
    } sva;

    /* Device Configuration Properties */
    uint32_t page_size;                 /* Host page size (from CC.MPS) */
    uint32_t page_bits;                 /* Page size in bits (log2) */

    /* Statistics */
    struct {
        uint64_t cmd_processed;         /* Total commands processed */
        uint64_t cmd_completed;         /* Total commands completed */
        uint64_t p2p_xfers;             /* Total P2P transfers */
        uint64_t p2p_bytes;             /* Total P2P bytes transferred */
        uint64_t dma_errors;            /* Total DMA errors */
        uint64_t cmd_errors;            /* Total command errors */
    } stats;

    /* Device Instance */
    uint16_t dev_id;                    /* Sequential device ID (assigned at realize) */

    /* Device Properties (from QEMU command line) */
    char *serial;                       /* Serial number */
    uint32_t cmb_size_mb;               /* Controller Memory Buffer size in MB */
};

/*
 * ===== Helper Functions =====
 *
 * Inline helper functions for queue management and state checking.
 */

/* Get completion queue for a request */
static inline AccelCQueue *accel_cq(AccelRequest *req)
{
    return req->sq->ctrl->cq[req->sq->cqid];
}

/* Increment CQ tail with wrap-around and phase toggle */
static inline void accel_inc_cq_tail(AccelCQueue *cq)
{
    cq->tail++;
    if (cq->tail >= cq->size) {
        cq->tail = 0;
        cq->phase = !cq->phase;  /* Toggle phase bit on wrap */
    }
}

/* Increment SQ head with wrap-around */
static inline void accel_inc_sq_head(AccelSQueue *sq)
{
    sq->head = (sq->head + 1) % sq->size;
}

/* Check if completion queue is full */
static inline bool accel_cq_full(AccelCQueue *cq)
{
    return (cq->tail + 1) % cq->size == cq->head;
}

/* Check if submission queue is empty */
static inline bool accel_sq_empty(AccelSQueue *sq)
{
    return sq->head == sq->tail;
}

/* Calculate number of pending entries in submission queue */
static inline uint32_t accel_sq_pending(AccelSQueue *sq)
{
    if (sq->tail >= sq->head) {
        return sq->tail - sq->head;
    } else {
        return sq->size - sq->head + sq->tail;
    }
}

/* Calculate number of pending entries in completion queue */
static inline uint32_t accel_cq_pending(AccelCQueue *cq)
{
    if (cq->tail >= cq->head) {
        return cq->tail - cq->head;
    } else {
        return cq->size - cq->head + cq->tail;
    }
}

/* Check if queue ID is valid */
static inline bool accel_check_sqid(PCIeAccel *n, uint16_t sqid)
{
    return sqid <= n->max_ioqpairs && n->sq != NULL && n->sq[sqid] != NULL;
}

static inline bool accel_check_cqid(PCIeAccel *n, uint16_t cqid)
{
    return cqid <= n->max_ioqpairs && n->cq != NULL && n->cq[cqid] != NULL;
}

/*
 * ===== Function Declarations =====
 *
 * Core device functions implemented in pcie-accelerator.c
 */

/* Device lifecycle */
void pcie_accel_realize(PCIDevice *pci_dev, Error **errp);
void pcie_accel_exit(PCIDevice *pci_dev);
void pcie_accel_reset(DeviceState *dev);

/* Queue management */
void accel_init_sq(AccelSQueue *sq, PCIeAccel *n, uint64_t dma_addr,
                   uint16_t sqid, uint16_t cqid, uint16_t size);
void accel_init_cq(AccelCQueue *cq, PCIeAccel *n, uint64_t dma_addr,
                   uint16_t cqid, uint16_t vector, uint16_t size,
                   uint16_t irq_enabled);
void accel_free_sq(AccelSQueue *sq, PCIeAccel *n);
void accel_free_cq(AccelCQueue *cq, PCIeAccel *n);

/* Command processing */
void accel_process_sq(void *opaque);
void accel_post_cqes(void *opaque);
void accel_enqueue_req_completion(AccelCQueue *cq, AccelRequest *req);

/* Interrupt handling (MSI/MSI-X only) */
void accel_irq_assert(PCIeAccel *n, AccelCQueue *cq);
void accel_irq_deassert(PCIeAccel *n, AccelCQueue *cq);

/* Admin command handlers */
uint16_t accel_admin_cmd(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_create_ioq(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_delete_ioq(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_identify(PCIeAccel *n, AccelRequest *req);

/* I/O command handlers */
uint16_t accel_io_cmd(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_loopback(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_mem_read(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_mem_write(PCIeAccel *n, AccelRequest *req);

/* P2P DMA functions (implemented in pcie-accelerator-p2p.c) */
uint16_t accel_cmd_p2p_setup(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_p2p_teardown(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_p2p_write(PCIeAccel *n, AccelRequest *req);
uint16_t accel_cmd_p2p_read(PCIeAccel *n, AccelRequest *req);
AccelP2PPeer *accel_find_p2p_peer(PCIeAccel *n, uint16_t bdf);
int accel_register_p2p_peer(PCIeAccel *n, uint16_t bdf, PCIDevice *pdev);
void accel_unregister_p2p_peer(PCIeAccel *n, uint16_t bdf);
size_t accel_p2p_get_stats(PCIeAccel *n, void *buf, size_t size);
void accel_p2p_dump_state(PCIeAccel *n);

/* P2P Ring Buffer functions (implemented in pcie-accelerator-p2p.c) */
void accel_p2p_ring_doorbell(PCIeAccel *n, hwaddr offset, uint32_t val);
void accel_process_p2p_ring(void *opaque);
void accel_p2p_ring_reset(PCIeAccel *n);
void accel_p2p_ring_cleanup(PCIeAccel *n);

/* Utility functions */
uint16_t accel_dma_read_safe(PCIeAccel *n, uint64_t addr, void *buf, size_t len);
uint16_t accel_dma_write_safe(PCIeAccel *n, uint64_t addr, const void *buf, size_t len);
void accel_set_ctrl_ready(PCIeAccel *n, bool ready);
void accel_set_ctrl_fatal(PCIeAccel *n);

#endif /* HW_PCIE_ACCELERATOR_H */
