/*
 * QEMU PCIe Accelerator Device - Register Definitions
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This file defines the register layout and constants for the PCIe Accelerator
 * virtual device. The device uses submission/completion queue management with
 * doorbell registers, supports PCIe P2P DMA, PASID/SVA, and MSI-X interrupts
 * with coalescing.
 */

#ifndef HW_PCIE_ACCELERATOR_REGS_H
#define HW_PCIE_ACCELERATOR_REGS_H

/*
 * ===== Controller Register Map =====
 *
 * BAR0 (64KB):  MMIO registers + doorbells
 * BAR2 (CMB):   Controller Memory Buffer (ring buffers + data)
 * BAR4 (16KB):  MSI-X table/PBA
 *
 * All multi-byte fields are little-endian.
 */

/* ===== Controller Capability Register (CAP) - Offset 0x0000 ===== */
/*
 * 64-bit read-only register describing device capabilities.
 *
 * Bit [0]     - P2P: Peer-to-Peer MMIO Queues Supported
 *                0 = P2P MMIO queues not supported
 *                1 = Device supports P2P MMIO queues
 *                Reset: 1 (supported)
 *
 * Bit [1]     - SVA: Shared Virtual Addressing Supported
 *                0 = PASID/SVA not supported
 *                1 = PASID/SVA supported via PCIe capability
 *                Reset: 1 (supported)
 *
 * Bit [2]     - PRPL: PRPL DMA supported
 *                0 = PRPL DMA not supported
 *                1 = PRPL DMA supported
 *                Reset: 1 (supported)
 *
 * Bit [3]     - SGL: SGL DMA supported
 *                0 = SGL DMA not supported
 *                1 = SGL DMA supported
 *                Reset: 1 (supported)
 *
 * Bits [7:4]  - P2P_CH_BS: P2P Channel Buffer Size
 *                Value N represent power of 2. 2^N x 4096B
 *                Reset: 0xC (4096B)
 * 
 * Bits [11:8]  - SQS: host SQ entry size
 *                Value N represent power of 2. 2^N
 *                Reset: 0x6 (64B)
 * 
 * Bits [15:12]  - CQS: host CQ entry size
 *                Value N represent power of 2. 2^N
 *                Reset: 0x4 (16B)
 * 
 * Bits [19:16]  - DEPTH: host SQ/CQ entry count
 *                Value N represent power of 2. 2^N
 *                Reset: 0x4 (16B)
 * 
 * Bits [23:20]  - MAXQ: max host CQ/SQ pair count
 *                Value N represent power of 2. 2^N
 *                Reset: 0x4 (16B)
 * 
 * Bits [27:24]  - MAXR: max P2P ring buffer count
 *                Value N represent power of 2. 2^N
 *                Reset: 0x4 (16B)
 * 
 * Bits [63:28] - Reserved (must be 0)
 */
#define ACCEL_REG_CAP       0x0000

#define ACCEL_CAP_P2P_SHIFT         0
#define ACCEL_CAP_SVA_SHIFT         1
#define ACCEL_CAP_PRPL_SHIFT        2
#define ACCEL_CAP_SGL_SHIFT         3
#define ACCEL_CAP_P2P_CH_BS_SHIFT   4
#define ACCEL_CAP_P2P_CH_BS_MASK    0xF
#define ACCEL_CAP_SQS_SHIFT         8
#define ACCEL_CAP_SQS_MASK          0xF
#define ACCEL_CAP_CQS_SHIFT         12
#define ACCEL_CAP_CQS_MASK          0xF
#define ACCEL_CAP_DEPTH_SHIFT       16
#define ACCEL_CAP_DEPTH_MASK        0xF
#define ACCEL_CAP_MAXQ_SHIFT        20
#define ACCEL_CAP_MAXQ_MASK         0xF
#define ACCEL_CAP_MAXR_SHIFT        24
#define ACCEL_CAP_MAXR_MASK         0xF

/* ===== Controller Configuration Register (CC) - Offset 0x0008 ===== */
/*
 * 32-bit read/write register for controller configuration.
 *
 * Bit [0]      - EN: Enable
 *                0 = Controller disabled, all I/O queues are suspended
 *                1 = Controller enabled, normal operation
 *                Reset: 0
 *
 * Bits [3:1]   - Reserved (must be 0)
 *
 * Bits [7:4]   - IOSQES: I/O Submission Queue Entry Size
 *                Entry size = 2^IOSQES bytes
 *                Must be 6 (64 bytes) for this device
 *                Reset: 6
 *
 * Bits [11:8]  - IOCQES: I/O Completion Queue Entry Size
 *                Entry size = 2^IOCQES bytes
 *                Must be 4 (16 bytes) for this device
 *                Reset: 4
 *
 * Bits [13:12] - MPS: Memory Page Size
 *                Host page size = 2^(12 + MPS) bytes
 *                0 = 4KB, 1 = 8KB, 2 = 16KB, 3 = 32KB
 *                Reset: 0 (4KB)
 *
 * Bits [31:14] - Reserved (must be 0)
 */
#define ACCEL_REG_CC        0x0008

#define ACCEL_CC_EN_SHIFT       0
#define ACCEL_CC_EN_MASK        0x1
#define ACCEL_CC_IOSQES_SHIFT   4
#define ACCEL_CC_IOSQES_MASK    0xF
#define ACCEL_CC_IOCQES_SHIFT   8
#define ACCEL_CC_IOCQES_MASK    0xF
#define ACCEL_CC_MPS_SHIFT      12
#define ACCEL_CC_MPS_MASK       0x3

/* Required entry sizes */
#define ACCEL_SQES          6   /* 2^6 = 64 bytes */
#define ACCEL_CQES          4   /* 2^4 = 16 bytes */

/* ===== Controller Status Register (CSTS) - Offset 0x000C ===== */
/*
 * 32-bit read-only register indicating controller status.
 *
 * Bit [0]      - RDY: Ready
 *                0 = Controller not ready to process commands
 *                1 = Controller ready to process submission queue entries
 *                Transitions to 1 after CC.EN set to 1 and initialization complete
 *                Transitions to 0 after CC.EN set to 0 and shutdown complete
 *
 * Bit [1]      - CFS: Controller Fatal Status
 *                0 = No fatal error
 *                1 = Fatal controller error occurred, requires reset
 *                When set, controller stops processing commands
 *
 * Bits [3:2]   - SHST: Shutdown Status
 *                00 = Normal operation
 *                01 = Shutdown processing occurring
 *                10 = Shutdown processing complete
 *                11 = Reserved
 *
 * Bit [4]      - SSRO: Subsystem Reset Occurred
 *                0 = No subsystem reset occurred
 *                1 = Subsystem reset occurred (informational)
 *
 * Bit [5]      - PP: Processing Paused
 *                0 = Command processing not paused
 *                1 = Command processing paused due to error or admin command
 *
 * Bits [31:6]  - Reserved (must be 0)
 */
#define ACCEL_REG_CSTS      0x000C

#define ACCEL_CSTS_RDY_SHIFT    0
#define ACCEL_CSTS_RDY_MASK     0x1
#define ACCEL_CSTS_CFS_SHIFT    1
#define ACCEL_CSTS_CFS_MASK     0x1
#define ACCEL_CSTS_SHST_SHIFT   2
#define ACCEL_CSTS_SHST_MASK    0x3
#define ACCEL_CSTS_SSRO_SHIFT   4
#define ACCEL_CSTS_PP_SHIFT     5

/* Shutdown status values */
#define ACCEL_CSTS_SHST_NORMAL  0x0
#define ACCEL_CSTS_SHST_OCCURRING 0x1
#define ACCEL_CSTS_SHST_COMPLETE 0x2

/* ===== Admin Submission Queue Base Address (ASQ) - Offset 0x0010 ===== */
/*
 * 64-bit read/write register specifying admin SQ base address.
 *
 * Bits [11:0]  - Reserved (must be 0, queue must be page-aligned)
 * Bits [63:12] - ASQB: Admin Submission Queue Base Address
 *                Physical base address of admin submission queue
 *                Must be aligned to host page size (MPS)
 *                Reset: 0
 */
#define ACCEL_REG_ASQ       0x0010

/* ===== Admin Completion Queue Base Address (ACQ) - Offset 0x0018 ===== */
/*
 * 64-bit read/write register specifying admin CQ base address.
 *
 * Bits [11:0]  - Reserved (must be 0, queue must be page-aligned)
 * Bits [63:12] - ACQB: Admin Completion Queue Base Address
 *                Physical base address of admin completion queue
 *                Must be aligned to host page size (MPS)
 *                Reset: 0
 */
#define ACCEL_REG_ACQ       0x0018

/* ===== CMB BAR Register (CMBBAR) - Offset 0x0020 ===== */
/*
 * 32-bit read-only register indicating which BAR contains the Controller
 * Memory Buffer (CMB).
 *
 * Bits [2:0]   - BAR: BAR number containing the CMB
 *                Reset: 2 (CMB is in BAR2)
 *
 * Bits [31:3]  - Reserved (must be 0)
 */
#define ACCEL_REG_CMBBAR    0x0020

/* ===== CMB Size Register (CMBSZ) - Offset 0x0024 ===== */
/*
 * 32-bit read-only register indicating the size of the Controller
 * Memory Buffer (CMB) in bytes.
 *
 * Bits [31:0]  - SIZE: CMB size in bytes
 *                Reset: ACCEL_CMB_SIZE
 */
#define ACCEL_REG_CMBSZ     0x0024

/* ===== P2P Configuration Register (P2PCFG) - Offset 0x0028 ===== */
/*
 * 32-bit read-only register describing P2P DMA capabilities.
 *
 * Bits [7:0]   - MAX_DEVICES: Maximum Concurrent P2P Peer Devices
 *                Number of peer devices that can be registered simultaneously
 *                Reset: 32
 *
 * Bits [15:8]  - MAX_XFERS: Maximum Concurrent P2P Transfers Per Device
 *                Number of concurrent transfers allowed to each peer
 *                Reset: 64
 *
 * Bit [16]     - ROUTING: P2P Routing Mode
 *                0 = Direct routing (peer-to-peer via switch)
 *                1 = Root complex routing (via RC)
 *                Reset: 0 (direct routing)
 *
 * Bits [31:17] - Reserved (must be 0)
 */
#define ACCEL_REG_P2PCFG    0x0028

#define ACCEL_P2PCFG_MAX_DEVICES_SHIFT  0
#define ACCEL_P2PCFG_MAX_DEVICES_MASK   0xFF
#define ACCEL_P2PCFG_MAX_XFERS_SHIFT    8
#define ACCEL_P2PCFG_MAX_XFERS_MASK     0xFF
#define ACCEL_P2PCFG_ROUTING_SHIFT      16

/* Default P2P limits */
#define ACCEL_MAX_P2P_PEERS     32
#define ACCEL_MAX_P2P_XFERS     64

/* ===== Interrupt Coalescing Configuration (INTCOAL) - Offset 0x002C ===== */
/*
 * 32-bit read/write register for interrupt coalescing parameters.
 *
 * Bits [7:0]   - THRESH: Interrupt Coalescing Threshold
 *                Number of completions before interrupt is generated
 *                0 = No threshold coalescing (interrupt per completion)
 *                1-255 = Fire interrupt after N completions
 *                Reset: 0 (no coalescing)
 *
 * Bits [15:8]  - TIME: Interrupt Coalescing Time
 *                Time threshold in 100 microsecond units
 *                0 = No time-based coalescing
 *                1-255 = Fire interrupt after N * 100us if pending completions
 *                Reset: 0 (no time coalescing)
 *
 * Bits [31:16] - Reserved (must be 0)
 */
#define ACCEL_REG_INTCOAL   0x002C

#define ACCEL_INTCOAL_THRESH_SHIFT  0
#define ACCEL_INTCOAL_THRESH_MASK   0xFF
#define ACCEL_INTCOAL_TIME_SHIFT    8
#define ACCEL_INTCOAL_TIME_MASK     0xFF

/* ===== Device Status Register (DEVSTAT) - Offset 0x0030 ===== */
/*
 * 32-bit read-only register providing real-time device statistics.
 *
 * Bits [7:0]   - NUM_PEERS: Current Number of Registered P2P Peers
 *                Number of peer devices currently registered for P2P DMA
 *                Range: 0 to MAX_DEVICES from P2PCFG
 *
 * Bits [15:8]  - ACTIVE_XFERS: Current Active P2P Transfers
 *                Total number of P2P transfers currently in progress
 *                Range: 0 to (NUM_PEERS * MAX_XFERS)
 *
 * Bits [23:16] - QUEUE_PAIRS: Current Number of I/O Queue Pairs
 *                Number of I/O queue pairs currently created
 *                Range: 0 to MQES
 *
 * Bits [31:24] - Reserved (must be 0)
 */
#define ACCEL_REG_DEVSTAT   0x0030

#define ACCEL_DEVSTAT_NUM_PEERS_SHIFT     0
#define ACCEL_DEVSTAT_NUM_PEERS_MASK      0xFF
#define ACCEL_DEVSTAT_ACTIVE_XFERS_SHIFT  8
#define ACCEL_DEVSTAT_ACTIVE_XFERS_MASK   0xFF
#define ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT   16
#define ACCEL_DEVSTAT_QUEUE_PAIRS_MASK    0xFF

/* ===== Doorbell Registers - Offset 0x1000 ===== */
/*
 * Doorbell registers for submission and completion queues.
 * Located at offset 0x1000 with 4-byte stride.
 *
 * Submission Queue Doorbell (32-bit write-only):
 *   Offset: 0x1000 + (2 * qid * stride)
 *   Bits [15:0]  - SQ_TAIL: New submission queue tail pointer
 *                  Write to this register advances SQ tail
 *   Bits [31:16] - Reserved (must be 0)
 *
 * Completion Queue Doorbell (32-bit write-only):
 *   Offset: 0x1000 + (2 * qid + 1) * stride
 *   Bits [15:0]  - CQ_HEAD: New completion queue head pointer
 *                  Write to this register advances CQ head (ACK completions)
 *   Bits [31:16] - Reserved (must be 0)
 *
 * Queue ID (qid) mapping:
 *   - qid 0: Admin queue
 *   - qid 1-N: I/O queues
 */
#define ACCEL_REG_DOORBELL  0x1000
#define ACCEL_DB_STRIDE     0x0004  /* 4 bytes per doorbell (stride=0) */

/*
 * Calculate doorbell register offset for a queue
 */
#define ACCEL_SQ_DOORBELL(qid, stride) \
    (ACCEL_REG_DOORBELL + (2 * (qid) * (stride)))
#define ACCEL_CQ_DOORBELL(qid, stride) \
    (ACCEL_REG_DOORBELL + ((2 * (qid) + 1) * (stride)))

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
 *   10 = HVA  (Host Virtual Address)
 *   11 = Reserved
 *
 * Bit [2] - PASID_EN: Enable PASID for this command
 * Bit [3] - PRIV: Privileged operation
 * Bits [7:4] - Reserved
 */
#define ACCEL_CMD_FLAGS_DBD_MASK        0x03
#define ACCEL_CMD_FLAGS_DBD_PRPL        0x00  /* PRP List */
#define ACCEL_CMD_FLAGS_DBD_SGL         0x01  /* Scatter-Gather List */
#define ACCEL_CMD_FLAGS_DBD_HVA         0x02  /* Host Virtual Address */
#define ACCEL_CMD_FLAGS_PASID_EN        0x04  /* PASID enable */
#define ACCEL_CMD_FLAGS_PRIV            0x08  /* Privileged operation */

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

/* ===== Feature Identifiers ===== */
/*
 * Feature IDs for Get/Set Features admin commands.
 */
#define ACCEL_FEAT_INTERRUPT_COALESCING 0x01  /* Interrupt coalescing settings */
#define ACCEL_FEAT_NUM_QUEUES           0x07  /* Number of queues */
#define ACCEL_FEAT_P2P_CONFIG           0x10  /* P2P configuration */

/* ===== P2P Ring Configuration Register (P2RCFG) - Offset 0x0034 ===== */
/*
 * 32-bit read-only register describing P2P ring buffer capabilities.
 *
 * Bits [3:0]   - SLOTS: Number of P2P ring buffer slots (0-7)
 *                Each slot supports one peer device.
 *                Reset: 7
 *
 * Bits [23:4]  - RING_SIZE: Per-slot ring size in 4KB units
 *                Reset: 256 (1MB)
 *
 * Bits [31:24] - Reserved (must be 0)
 */
#define ACCEL_REG_P2RCFG                0x0034

#define ACCEL_P2RCFG_SLOTS_SHIFT        0
#define ACCEL_P2RCFG_SLOTS_MASK         0xF
#define ACCEL_P2RCFG_RING_SIZE_SHIFT    4
#define ACCEL_P2RCFG_RING_SIZE_MASK     0xFFFFF

/* ===== P2P Ring Doorbell Registers - Offset 0x4000 ===== */
/*
 * Doorbell registers for P2P ring buffers between devices.
 * These are written by PEER devices via cross-device MMIO writes
 * (address_space_write to this device's BAR0).
 *
 * Per peer slot (8 bytes per slot):
 *   Offset 0x4000 + slot*8 + 0: Ring Tail Doorbell (32-bit write-only)
 *     Written by peer after producing messages in our inbound ring.
 *     Bits [31:0] = new tail byte offset within ring data area.
 *
 *   Offset 0x4000 + slot*8 + 4: Reserved
 */
#define ACCEL_P2R_DB_BASE               0x4000
#define ACCEL_P2R_DB_STRIDE             8

#define ACCEL_P2R_TAIL_DB(slot) \
    (ACCEL_P2R_DB_BASE + (slot) * ACCEL_P2R_DB_STRIDE)

/* ===== P2P Ring Constants ===== */
#define ACCEL_P2R_MAX_SLOTS             7       /* Max peer ring slots */

/* Ring buffer sizes (per slot) */
#define ACCEL_RING_SIZE                 (1 * 1024 * 1024)  /* 1MB per ring */
#define ACCEL_RING_HDR_SIZE             64      /* Ring header size in bytes */
#define ACCEL_RING_DATA_OFFSET          ACCEL_RING_HDR_SIZE
#define ACCEL_RING_DATA_SIZE            (ACCEL_RING_SIZE - ACCEL_RING_HDR_SIZE)

/* Ring layout within CMB (offsets relative to CMB start) */
#define ACCEL_RING_OFFSET(slot)         ((slot) * ACCEL_RING_SIZE)
#define ACCEL_CMB_DATA_OFFSET           (ACCEL_P2R_MAX_SLOTS * ACCEL_RING_SIZE)

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

/* ===== P2P Ring Status Codes ===== */
#define ACCEL_SC_P2R_INVALID_SLOT       0x50  /* Invalid ring slot */
#define ACCEL_SC_P2R_SLOT_ACTIVE        0x51  /* Slot already in use */
#define ACCEL_SC_P2R_PEER_MISMATCH      0x52  /* Peer device type mismatch */

/* ===== BAR Sizes ===== */
#define ACCEL_BAR0_SIZE         (8 * 1024)         /* 8KB - MMIO registers + doorbells */
#define ACCEL_CMB_SIZE          (32 * 1024 * 1024)   /* 16MB - Controller Memory Buffer (BAR2) */
#define ACCEL_BAR4_SIZE         (16 * 1024)         /* 16KB - MSI-X table/PBA */

/* MSI-X table/PBA offsets within BAR4 */
#define ACCEL_MSIX_TABLE_OFFSET     0x0000  /* MSI-X table at BAR4 offset 0 */
#define ACCEL_MSIX_PBA_OFFSET       0x1000  /* MSI-X PBA at BAR4 offset 4KB */

/* ===== Maximum Values ===== */
#define ACCEL_MAX_IOQPAIRS      256     /* Maximum I/O queue pairs */
#define ACCEL_MAX_QUEUE_ENTRIES 4096    /* Maximum entries per queue */
#define ACCEL_MIN_QUEUE_ENTRIES 2       /* Minimum entries per queue */
#define ACCEL_ADMIN_QUEUE_SIZE  64      /* Fixed admin queue size (entries) */

/* ===== PCIe Configuration ===== */
#define ACCEL_PCIE_VENDOR_ID    0x1234  /* QEMU vendor ID (example) */
#define ACCEL_PCIE_DEVICE_ID    0x5678  /* PCIe Accelerator device ID (example) */
#define ACCEL_PCIE_CLASS        0x0880  /* System peripheral: Other */
#define ACCEL_PCIE_REVISION     0x01    /* Revision 1 */

#endif /* HW_PCIE_ACCELERATOR_REGS_H */
