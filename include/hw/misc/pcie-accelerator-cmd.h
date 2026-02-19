/*
 * QEMU PCIe Accelerator Device - Command Definitions
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This file defines the command set opcodes, flags, status codes, and
 * related constants for the PCIe Accelerator device.
 */

#ifndef HW_PCIE_ACCELERATOR_CMD_H
#define HW_PCIE_ACCELERATOR_CMD_H

/* ===== Command Set Opcodes ===== */
/*
 * Command opcodes for submission queue entries.
 * Organized into admin and I/O command sets.
 */

/* Admin Command Set */
#define ACCEL_ADM_CMD_IDENTIFY          0x00
#define ACCEL_ADM_CMD_SET_FEATURES      0x09
#define ACCEL_ADM_CMD_GET_FEATURES      0x0A
#define ACCEL_ADM_CMD_CREATE_IOQ        0x0D
#define ACCEL_ADM_CMD_DELETE_IOQ        0x0E
#define ACCEL_ADM_CMD_P2P_SETUP         0x0F
#define ACCEL_ADM_CMD_P2P_TEARDOWN      0x10

/* I/O Command Set */
#define ACCEL_CMD_LOOPBACK              0x01
#define ACCEL_CMD_P2P_WRITE             0x02
#define ACCEL_CMD_P2P_READ              0x03
#define ACCEL_CMD_MEM_READ              0x05
#define ACCEL_CMD_MEM_WRITE             0x06

/* ===== Command Flags ===== */
/*
 * Flags for command submission (cmd.flags field).
 *
 * Bits [1:0] - DBD_TYPE: Data Block Descriptor type
 *   00 = PRPL (Physical Region Page List)
 *   01 = SGL  (Scatter-Gather List)
 *   10 = SVA  (Shared Virtual Address)
 *   11 = Reserved
 *
 * Bit [2] - PASID_EN: Enable PASID for this command
 * Bit [3] - PRIV: Privileged operation
 * Bits [7:4] - Reserved
 */
#define ACCEL_CMD_FLAGS_DBD_MASK        0x03
#define ACCEL_CMD_FLAGS_DBD_PRPL        0x00  /* PRP List */
#define ACCEL_CMD_FLAGS_DBD_SGL         0x01  /* Scatter-Gather List */
#define ACCEL_CMD_FLAGS_DBD_SVA         0x02  /* Shared Virtual Address */
#define ACCEL_CMD_FLAGS_PASID_EN        0x04  /* PASID enable */
#define ACCEL_CMD_FLAGS_PRIV            0x08  /* Privileged operation */

/* SGL Descriptor Types (dbd.sgl.type[7:0]) */
#define ACCEL_SGL_DESC_DATA_BLOCK       0x00  /* Data Block Descriptor */
#define ACCEL_SGL_DESC_SEGMENT          0x02  /* Segment Descriptor (chain) */
#define ACCEL_SGL_DESC_LAST_SEGMENT     0x03  /* Last Segment Descriptor */

/* ===== Completion Status Codes ===== */
/*
 * Status codes returned in completion queue entries.
 * Generic status codes (0x00-0x0F): Common across all commands
 * Command-specific codes (0x10-0xFF): Specific to command type
 */

/* Generic Success */
#define ACCEL_SC_SUCCESS                0x00  /* Command completed successfully */

/* Generic Command Status (0x01-0x0F) */
#define ACCEL_SC_INVALID_OPCODE         0x01  /* Invalid command opcode */
#define ACCEL_SC_INVALID_FIELD          0x02  /* Invalid field in command */
#define ACCEL_SC_CID_CONFLICT           0x03  /* Command ID conflict */
#define ACCEL_SC_DATA_XFER_ERROR        0x04  /* Data transfer error */
#define ACCEL_SC_CMD_ABORTED            0x05  /* Command aborted by request */
#define ACCEL_SC_INTERNAL_ERROR         0x06  /* Internal device error */
#define ACCEL_SC_CMD_ABORT_REQ          0x07  /* Command abort requested */
#define ACCEL_SC_CMD_ABORT_SQID         0x08  /* Command aborted due to SQ deletion */
#define ACCEL_SC_FUSED_FAIL             0x09  /* Fused command failed */
#define ACCEL_SC_FUSED_MISSING          0x0A  /* Missing fused command */
#define ACCEL_SC_INVALID_NAMESPACE      0x0B  /* Invalid namespace/peer ID */
#define ACCEL_SC_CMD_SEQ_ERROR          0x0C  /* Command sequence error */

/* Command Specific Status - Admin Commands (0x10-0x1F) */
#define ACCEL_SC_INVALID_QUEUE_ID       0x10  /* Invalid queue identifier */
#define ACCEL_SC_INVALID_QUEUE_SIZE     0x11  /* Invalid queue size */
#define ACCEL_SC_INVALID_QUEUE_ADDR     0x12  /* Invalid queue address alignment */
#define ACCEL_SC_MAX_QUEUES_EXCEEDED    0x13  /* Maximum queue limit exceeded */
#define ACCEL_SC_QUEUE_ALREADY_EXISTS   0x14  /* Queue already created */
#define ACCEL_SC_INVALID_IRQ_VECTOR     0x15  /* Invalid interrupt vector */
#define ACCEL_SC_INVALID_LOG_PAGE       0x16  /* Invalid log page */
#define ACCEL_SC_FEATURE_NOT_SUPPORTED  0x17  /* Feature not supported */
#define ACCEL_SC_FEATURE_NOT_CHANGEABLE 0x18  /* Feature not changeable */
#define ACCEL_SC_FEATURE_NOT_NAMESPACE  0x19  /* Feature not namespace specific */

/* Command Specific Status - P2P Commands (0x20-0x2F) */
#define ACCEL_SC_P2P_NOT_SUPPORTED      0x20  /* P2P DMA not supported */
#define ACCEL_SC_P2P_PEER_NOT_FOUND     0x21  /* Peer device not registered */
#define ACCEL_SC_P2P_PEER_INVALID       0x22  /* Invalid peer device */
#define ACCEL_SC_P2P_XFER_ERROR         0x23  /* P2P transfer error */
#define ACCEL_SC_P2P_MAX_PEERS          0x24  /* Maximum peers exceeded */
#define ACCEL_SC_P2P_MAX_XFERS          0x25  /* Maximum concurrent transfers exceeded */
#define ACCEL_SC_P2P_ADDR_INVALID       0x26  /* Invalid peer address */
#define ACCEL_SC_P2P_LEN_INVALID        0x27  /* Invalid transfer length */

/* Command Specific Status - DMA/Memory (0x30-0x3F) */
#define ACCEL_SC_DMA_ERROR              0x30  /* DMA operation failed */
#define ACCEL_SC_DMA_DECODE_ERROR       0x31  /* DMA address decode error */
#define ACCEL_SC_DMA_TIMEOUT            0x32  /* DMA operation timeout */
#define ACCEL_SC_INVALID_PRP            0x33  /* Invalid PRP/buffer pointer */
#define ACCEL_SC_PRP_OFFSET_INVALID     0x34  /* PRP offset invalid */
#define ACCEL_SC_LBA_OUT_OF_RANGE       0x35  /* Address out of range */

/* Command Specific Status - PASID/SVA (0x40-0x4F) */
#define ACCEL_SC_PASID_NOT_SUPPORTED    0x40  /* PASID not supported */
#define ACCEL_SC_PASID_INVALID          0x41  /* Invalid PASID value */
#define ACCEL_SC_PASID_NOT_ENABLED      0x42  /* PASID not enabled */
#define ACCEL_SC_SVA_FAULT              0x43  /* Shared virtual address fault */

/* Status Code Type */
#define ACCEL_SCT_GENERIC               0x0   /* Generic command status */
#define ACCEL_SCT_SPECIFIC              0x1   /* Command specific status */
#define ACCEL_SCT_MEDIA_ERROR           0x2   /* Media and data integrity errors */
#define ACCEL_SCT_VENDOR                0x7   /* Vendor specific */

/* Do Not Retry (DNR) bit */
#define ACCEL_SC_DNR                    (1 << 15)  /* Do not retry this command */

/* ===== P2P Ring Status Codes ===== */
#define ACCEL_SC_P2R_INVALID_SLOT       0x50  /* Invalid ring slot */
#define ACCEL_SC_P2R_SLOT_ACTIVE        0x51  /* Slot already in use */
#define ACCEL_SC_P2R_PEER_MISMATCH      0x52  /* Peer device type mismatch */

/* ===== Feature Identifiers ===== */
/*
 * Feature IDs for Get/Set Features admin commands.
 */
#define ACCEL_FEAT_INTERRUPT_COALESCING 0x01  /* Interrupt coalescing settings */
#define ACCEL_FEAT_NUM_QUEUES           0x07  /* Number of queues */
#define ACCEL_FEAT_P2P_CONFIG           0x10  /* P2P configuration */

/* ===== Ring Message Format ===== */
/*
 * Variable-length messages in ring data area.
 * Each message is 8-byte aligned.
 *
 * struct AccelRingMsg {
 *     uint16_t type;      // message type
 *     uint16_t flags;     // per-message flags
 *     uint32_t length;    // total length including header (8-byte aligned)
 *     uint8_t  payload[]; // variable payload
 * };
 */
#define ACCEL_RING_MSG_HDR_SIZE         8       /* Minimum message size */
#define ACCEL_RING_MSG_ALIGN            8       /* Message alignment */

/* Ring message types */
#define ACCEL_RING_MSG_DATA             0x01    /* Data payload follows */
#define ACCEL_RING_MSG_NOTIFY           0x02    /* Signal/fence, no payload */
#define ACCEL_RING_MSG_STATUS           0x03    /* Status/completion response */

/* ===== Command and Completion Entry Structures ===== */
/*
 * These structures define the format of submission and completion queue entries.
 * They are shared between the device and driver, so layout must be exact.
 */

/* Maximum memory regions in identify data */
#define ACCEL_ID_MAX_MEM_REGIONS    191

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
            struct { uint64_t addr; uint32_t length; uint8_t reserved[3]; uint8_t type; } sgl;
            struct { uint64_t addr; uint32_t pasid; uint32_t reserved; } sva;
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

    /*
     * ===== Device Physical Address (DPA) Memory =====
     *
     * DPA is a separate address space from MMIO/CMB — never exposed on
     * any BAR.  Accessible only via MEM_READ/MEM_WRITE commands and as
     * the P2P peer_addr target.  Addresses start at 0 and extend to the
     * backend memory size.
     */

    /* Memory Read command (opcode 0x05) */
    struct QEMU_PACKED {
        uint8_t  opcode;
        uint8_t  flags;
        uint16_t cid;
        uint64_t dev_addr;         /* DPA offset in device memory */
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
        uint64_t dev_addr;         /* DPA offset in device memory */
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
    uint8_t reserved0[14];
    uint16_t ccnt;
    uint16_t pcnt;
    uint8_t reserved1[2];
    uint64_t cmem;
    uint32_t csmem;
    uint8_t  reserved2[984];
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

#endif /* HW_PCIE_ACCELERATOR_CMD_H */
