/*
 * QEMU PCIe Accelerator Device - Register Definitions
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This file defines the register layout and constants for the PCIe Accelerator
 * virtual device. The device follows NVMe-style queue management with doorbell
 * registers, supports PCIe P2P DMA, PASID/SVA, and MSI-X interrupts with coalescing.
 */

#ifndef HW_PCIE_ACCELERATOR_REGS_H
#define HW_PCIE_ACCELERATOR_REGS_H

/*
 * ===== Controller Register Map (BAR0 - 64KB MMIO) =====
 *
 * The register layout follows NVMe specification patterns for familiarity
 * and proven design. All multi-byte fields are little-endian.
 */

/* ===== Controller Capability Register (CAP) - Offset 0x0000 ===== */
/*
 * 64-bit read-only register describing device capabilities.
 *
 * Bits [15:0]  - MQES: Maximum Queue Entries Supported
 *                0-based value (e.g., 0x3F = 64 entries, 0xFFF = 4096 entries)
 *                Reset: 0xFFF (4096 entries max)
 *
 * Bit [16]     - CQR: Contiguous Queues Required
 *                0 = Queues can be non-contiguous in physical memory
 *                1 = Queues must be physically contiguous
 *                Reset: 0 (non-contiguous supported)
 *
 * Bits [23:17] - Reserved (must be 0)
 *
 * Bits [31:24] - TO: Timeout
 *                Controller ready timeout in 500ms units
 *                0 = no timeout, >0 = timeout value
 *                Reset: 0x01 (500ms)
 *
 * Bits [35:32] - DSTRD: Doorbell Stride
 *                Doorbell stride = 2^(2 + DSTRD) bytes
 *                0 = 4 bytes, 1 = 8 bytes, etc.
 *                Reset: 0 (4 bytes stride)
 *
 * Bit [36]     - P2P: Peer-to-Peer DMA Supported
 *                0 = P2P not supported
 *                1 = P2P DMA supported
 *                Reset: 1 (supported)
 *
 * Bit [37]     - SVA: Shared Virtual Addressing Supported
 *                0 = PASID/SVA not supported
 *                1 = PASID/SVA supported via PCIe capability
 *                Reset: 1 (supported)
 *
 * Bit [38]     - P2Q: P2P MMIO Queues Supported
 *                0 = P2P MMIO queues not supported
 *                1 = Device supports NVMe-style P2P MMIO queues
 *                Reset: 1 (supported)
 *
 * Bits [47:39] - Reserved (must be 0)
 *
 * Bits [51:48] - MPSMIN: Memory Page Size Minimum
 *                Minimum host memory page size = 2^(12 + MPSMIN) bytes
 *                0 = 4KB, 1 = 8KB, 2 = 16KB, etc.
 *                Reset: 0 (4KB minimum)
 *
 * Bits [55:52] - MPSMAX: Memory Page Size Maximum
 *                Maximum host memory page size = 2^(12 + MPSMAX) bytes
 *                0 = 4KB, 1 = 8KB, 2 = 16KB, etc.
 *                Reset: 8 (1MB maximum)
 *
 * Bits [63:56] - Reserved (must be 0)
 */
#define ACCEL_REG_CAP       0x0000

#define ACCEL_CAP_MQES_SHIFT    0
#define ACCEL_CAP_MQES_MASK     0xFFFF
#define ACCEL_CAP_CQR_SHIFT     16
#define ACCEL_CAP_TO_SHIFT      24
#define ACCEL_CAP_TO_MASK       0xFF
#define ACCEL_CAP_DSTRD_SHIFT   32
#define ACCEL_CAP_DSTRD_MASK    0xF
#define ACCEL_CAP_P2P_SHIFT     36
#define ACCEL_CAP_SVA_SHIFT     37
#define ACCEL_CAP_P2Q_SHIFT     38
#define ACCEL_CAP_MPSMIN_SHIFT  48
#define ACCEL_CAP_MPSMIN_MASK   0xF
#define ACCEL_CAP_MPSMAX_SHIFT  52
#define ACCEL_CAP_MPSMAX_MASK   0xF

/* ===== Version Register (VS) - Offset 0x0008 ===== */
/*
 * 32-bit read-only register indicating device version.
 *
 * Bits [7:0]   - TER: Tertiary Version Number
 * Bits [15:8]  - MNR: Minor Version Number
 * Bits [31:16] - MJR: Major Version Number
 *
 * Reset: 0x00010000 (version 1.0.0)
 */
#define ACCEL_REG_VS        0x0008
#define ACCEL_VS_TER_SHIFT  0
#define ACCEL_VS_MNR_SHIFT  8
#define ACCEL_VS_MJR_SHIFT  16

/* ===== Interrupt Mask Set Register (INTMS) - Offset 0x000C ===== */
/*
 * 32-bit read/write register for masking interrupts.
 * Write 1 to bit N to mask interrupt vector N.
 * Only applies to legacy INTx interrupts (MSI-X uses per-vector masking).
 *
 * Reset: 0x00000000 (all unmasked)
 */
#define ACCEL_REG_INTMS     0x000C

/* ===== Interrupt Mask Clear Register (INTMC) - Offset 0x0010 ===== */
/*
 * 32-bit write-only register for unmasking interrupts.
 * Write 1 to bit N to unmask interrupt vector N.
 */
#define ACCEL_REG_INTMC     0x0010

/* ===== Controller Configuration Register (CC) - Offset 0x0014 ===== */
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
 *                0 = 4KB, 1 = 8KB, 2 = 16KB, 3 = 32KB, etc.
 *                Must be between MPSMIN and MPSMAX from CAP register
 *                Reset: 0 (4KB)
 *
 * Bits [31:14] - Reserved (must be 0)
 */
#define ACCEL_REG_CC        0x0014

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

/* ===== Controller Status Register (CSTS) - Offset 0x001C ===== */
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
 * Bit [4]      - NSSRO: NVM Subsystem Reset Occurred
 *                0 = No subsystem reset occurred
 *                1 = Subsystem reset occurred (informational)
 *
 * Bit [5]      - PP: Processing Paused
 *                0 = Command processing not paused
 *                1 = Command processing paused due to error or admin command
 *
 * Bits [31:6]  - Reserved (must be 0)
 */
#define ACCEL_REG_CSTS      0x001C

#define ACCEL_CSTS_RDY_SHIFT    0
#define ACCEL_CSTS_RDY_MASK     0x1
#define ACCEL_CSTS_CFS_SHIFT    1
#define ACCEL_CSTS_CFS_MASK     0x1
#define ACCEL_CSTS_SHST_SHIFT   2
#define ACCEL_CSTS_SHST_MASK    0x3
#define ACCEL_CSTS_NSSRO_SHIFT  4
#define ACCEL_CSTS_PP_SHIFT     5

/* Shutdown status values */
#define ACCEL_CSTS_SHST_NORMAL  0x0
#define ACCEL_CSTS_SHST_OCCURRING 0x1
#define ACCEL_CSTS_SHST_COMPLETE 0x2

/* ===== Admin Queue Attributes Register (AQA) - Offset 0x0024 ===== */
/*
 * 32-bit read/write register defining admin queue sizes.
 *
 * Bits [11:0]  - ASQS: Admin Submission Queue Size
 *                0-based value (e.g., 0x3F = 64 entries)
 *                Minimum: 1 (2 entries), Maximum: MQES from CAP
 *                Reset: 0
 *
 * Bits [15:12] - Reserved (must be 0)
 *
 * Bits [27:16] - ACQS: Admin Completion Queue Size
 *                0-based value (e.g., 0x3F = 64 entries)
 *                Minimum: 1 (2 entries), Maximum: MQES from CAP
 *                Reset: 0
 *
 * Bits [31:28] - Reserved (must be 0)
 */
#define ACCEL_REG_AQA       0x0024

#define ACCEL_AQA_ASQS_SHIFT    0
#define ACCEL_AQA_ASQS_MASK     0xFFF
#define ACCEL_AQA_ACQS_SHIFT    16
#define ACCEL_AQA_ACQS_MASK     0xFFF

/* ===== Admin Submission Queue Base Address (ASQ) - Offset 0x0028 ===== */
/*
 * 64-bit read/write register specifying admin SQ base address.
 *
 * Bits [11:0]  - Reserved (must be 0, queue must be page-aligned)
 * Bits [63:12] - ASQB: Admin Submission Queue Base Address
 *                Physical base address of admin submission queue
 *                Must be aligned to host page size (MPS)
 *                Reset: 0
 */
#define ACCEL_REG_ASQ       0x0028

/* ===== Admin Completion Queue Base Address (ACQ) - Offset 0x0030 ===== */
/*
 * 64-bit read/write register specifying admin CQ base address.
 *
 * Bits [11:0]  - Reserved (must be 0, queue must be page-aligned)
 * Bits [63:12] - ACQB: Admin Completion Queue Base Address
 *                Physical base address of admin completion queue
 *                Must be aligned to host page size (MPS)
 *                Reset: 0
 */
#define ACCEL_REG_ACQ       0x0030

/* ===== P2P Configuration Register (P2PCFG) - Offset 0x0040 ===== */
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
#define ACCEL_REG_P2PCFG    0x0040

#define ACCEL_P2PCFG_MAX_DEVICES_SHIFT  0
#define ACCEL_P2PCFG_MAX_DEVICES_MASK   0xFF
#define ACCEL_P2PCFG_MAX_XFERS_SHIFT    8
#define ACCEL_P2PCFG_MAX_XFERS_MASK     0xFF
#define ACCEL_P2PCFG_ROUTING_SHIFT      16

/* Default P2P limits */
#define ACCEL_MAX_P2P_PEERS     32
#define ACCEL_MAX_P2P_XFERS     64

/* ===== Interrupt Coalescing Configuration (INTCOAL) - Offset 0x0048 ===== */
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
#define ACCEL_REG_INTCOAL   0x0050

#define ACCEL_INTCOAL_THRESH_SHIFT  0
#define ACCEL_INTCOAL_THRESH_MASK   0xFF
#define ACCEL_INTCOAL_TIME_SHIFT    8
#define ACCEL_INTCOAL_TIME_MASK     0xFF

/* ===== Device Status Register (DEVSTAT) - Offset 0x0058 ===== */
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
#define ACCEL_REG_DEVSTAT   0x0058

#define ACCEL_DEVSTAT_NUM_PEERS_SHIFT     0
#define ACCEL_DEVSTAT_NUM_PEERS_MASK      0xFF
#define ACCEL_DEVSTAT_ACTIVE_XFERS_SHIFT  8
#define ACCEL_DEVSTAT_ACTIVE_XFERS_MASK   0xFF
#define ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT   16
#define ACCEL_DEVSTAT_QUEUE_PAIRS_MASK    0xFF

/* ===== Doorbell Registers - Offset 0x1000 ===== */
/*
 * Doorbell registers for submission and completion queues.
 * Located at offset 0x1000 with stride defined by CAP.DSTRD.
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
#define ACCEL_ADM_CMD_DELETE_SQ         0x00
#define ACCEL_ADM_CMD_CREATE_SQ         0x01
#define ACCEL_ADM_CMD_DELETE_CQ         0x04
#define ACCEL_ADM_CMD_CREATE_CQ         0x05
#define ACCEL_ADM_CMD_IDENTIFY          0x06
#define ACCEL_ADM_CMD_GET_FEATURES      0x0A
#define ACCEL_ADM_CMD_SET_FEATURES      0x09
#define ACCEL_ADM_CMD_P2P_SETUP         0x10
#define ACCEL_ADM_CMD_P2P_TEARDOWN      0x11
#define ACCEL_ADM_CMD_P2P_QUEUE_SETUP   0x12
#define ACCEL_ADM_CMD_P2P_QUEUE_TEARDOWN 0x13

/* I/O Command Set */
#define ACCEL_CMD_LOOPBACK              0x01
#define ACCEL_CMD_P2P_WRITE             0x02
#define ACCEL_CMD_P2P_READ              0x03

/* ===== Command Flags ===== */
/*
 * Flags for command submission (cmd.flags field).
 */
#define ACCEL_CMD_FLAG_PASID_ENABLE     (1 << 0)  /* Enable PASID for this command */
#define ACCEL_CMD_FLAG_PRIV             (1 << 1)  /* Privileged operation */
#define ACCEL_CMD_FLAG_EXEC             (1 << 2)  /* Execute permission required */

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

/* ===== P2P Queue Configuration Register (P2QQCFG) - Offset 0x0060 ===== */
/*
 * 32-bit read-only register describing P2P MMIO queue capabilities.
 *
 * Bits [3:0]   - P2Q_SLOTS: Number of P2P queue pair slots (0-7)
 *                Each slot supports one peer device.
 *                Reset: 7
 *
 * Bits [15:4]  - P2Q_SIZE: P2P queue size in entries per queue
 *                Both SQ and CQ use the same size.
 *                Reset: 64
 *
 * Bits [31:16] - P2Q_DATA_SIZE: Data region size in 4KB units
 *                Amount of BAR2 space available for data transfers.
 *                Reset: 52 (208KB)
 */
#define ACCEL_REG_P2QQCFG               0x0060

#define ACCEL_P2QQCFG_SLOTS_SHIFT       0
#define ACCEL_P2QQCFG_SLOTS_MASK        0xF
#define ACCEL_P2QQCFG_SIZE_SHIFT        4
#define ACCEL_P2QQCFG_SIZE_MASK         0xFFF
#define ACCEL_P2QQCFG_DATA_SIZE_SHIFT   16
#define ACCEL_P2QQCFG_DATA_SIZE_MASK    0xFFFF

/* ===== P2P Queue Doorbell Registers - Offset 0x4000 ===== */
/*
 * Doorbell registers for P2P MMIO queues between devices.
 * These are written by PEER devices via cross-device MMIO writes
 * (address_space_write to this device's BAR0).
 *
 * Per peer slot (8 bytes per slot):
 *   Offset 0x4000 + slot*8 + 0: Inbound SQ Tail Doorbell (32-bit write-only)
 *     Written by peer after submitting commands to our inbound SQ.
 *     Bits [15:0] = new SQ tail value.
 *
 *   Offset 0x4000 + slot*8 + 4: Receive CQ Notify Doorbell (32-bit write-only)
 *     Written by peer after pushing CQE to our receive CQ.
 *     Bits [15:0] = new CQ tail value.
 */
#define ACCEL_P2Q_DB_BASE               0x4000
#define ACCEL_P2Q_DB_STRIDE             8

#define ACCEL_P2Q_SQ_TAIL_DB(slot) \
    (ACCEL_P2Q_DB_BASE + (slot) * ACCEL_P2Q_DB_STRIDE)
#define ACCEL_P2Q_CQ_NOTIFY_DB(slot) \
    (ACCEL_P2Q_DB_BASE + (slot) * ACCEL_P2Q_DB_STRIDE + 4)

/* ===== P2P Queue Constants ===== */
#define ACCEL_P2Q_MAX_SLOTS             7       /* Max peer queue slots */
#define ACCEL_P2Q_SQ_ENTRIES            64      /* Entries per inbound SQ */
#define ACCEL_P2Q_CQ_ENTRIES            64      /* Entries per receive CQ */

/* BAR2 layout for P2P queues + data */
#define ACCEL_P2Q_SQ_OFFSET(slot)       ((slot) * 0x1000)       /* 4KB per SQ */
#define ACCEL_P2Q_SQ_SIZE               (ACCEL_P2Q_SQ_ENTRIES * 64)  /* 4KB */
#define ACCEL_P2Q_CQ_BASE               0x8000
#define ACCEL_P2Q_CQ_OFFSET(slot)       (ACCEL_P2Q_CQ_BASE + (slot) * 0x400) /* 1KB per CQ */
#define ACCEL_P2Q_CQ_SIZE               (ACCEL_P2Q_CQ_ENTRIES * 16)  /* 1KB */
#define ACCEL_P2Q_DATA_OFFSET           0xC000  /* Start of data/scratchpad region */

/* ===== P2P Queue Command Opcodes ===== */
/*
 * Commands submitted via P2P MMIO queues (device-to-device).
 * These use a separate opcode range (0x80+) from host I/O commands.
 */
#define ACCEL_P2Q_CMD_MMIO_WRITE        0x80  /* Transfer data: submitter -> target */
#define ACCEL_P2Q_CMD_MMIO_READ         0x81  /* Transfer data: target -> submitter */
#define ACCEL_P2Q_CMD_LOOPBACK          0x82  /* Target loopback test */

/* ===== P2P Queue Status Codes ===== */
#define ACCEL_SC_P2Q_INVALID_SLOT       0x50  /* Invalid P2P queue slot */
#define ACCEL_SC_P2Q_SLOT_ACTIVE        0x51  /* Slot already in use */
#define ACCEL_SC_P2Q_PEER_MISMATCH      0x52  /* Peer device type mismatch */
#define ACCEL_SC_P2Q_DATA_RANGE         0x53  /* Data offset out of range */
#define ACCEL_SC_P2Q_XFER_ERROR         0x54  /* Cross-device MMIO transfer error */

/* ===== BAR Sizes and Offsets ===== */
#define ACCEL_BAR0_SIZE     (64 * 1024)   /* 64KB - Controller registers */
#define ACCEL_BAR2_SIZE     (256 * 1024)  /* 256KB - P2P queues + data RAM */
#define ACCEL_BAR4_SIZE     (16 * 1024)   /* 16KB - MSI-X table/PBA */

/* MSI-X table/PBA offsets within BAR4 */
#define ACCEL_MSIX_TABLE_OFFSET     0x0000  /* MSI-X table at BAR4 offset 0 */
#define ACCEL_MSIX_PBA_OFFSET       0x1000  /* MSI-X PBA at BAR4 offset 4KB */

/* ===== Maximum Values ===== */
#define ACCEL_MAX_IOQPAIRS      256     /* Maximum I/O queue pairs */
#define ACCEL_MAX_QUEUE_ENTRIES 4096    /* Maximum entries per queue */
#define ACCEL_MIN_QUEUE_ENTRIES 2       /* Minimum entries per queue */

/* ===== PCIe Configuration ===== */
#define ACCEL_PCIE_VENDOR_ID    0x1234  /* QEMU vendor ID (example) */
#define ACCEL_PCIE_DEVICE_ID    0x5678  /* PCIe Accelerator device ID (example) */
#define ACCEL_PCIE_CLASS        0x0880  /* System peripheral: Other */
#define ACCEL_PCIE_REVISION     0x01    /* Revision 1 */

#endif /* HW_PCIE_ACCELERATOR_REGS_H */
