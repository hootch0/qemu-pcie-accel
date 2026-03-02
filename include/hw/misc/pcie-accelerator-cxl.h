/*
 * QEMU PCIe Accelerator Device - CXL Type 1 Extensions
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This header defines the CXL Type 1 (CXL.io + CXL.cache) variant of the
 * PCIe Accelerator device.  Type 1 devices coherently cache host memory
 * using the CXL.cache protocol without exposing device-attached memory
 * (unlike Type 3 which uses CXL.mem).
 */

#ifndef HW_PCIE_ACCELERATOR_CXL_H
#define HW_PCIE_ACCELERATOR_CXL_H

#include "hw/misc/pcie-accelerator.h"
#include "hw/cxl/cxl.h"

#define TYPE_PCIE_ACCEL_CXL "pcie-accelerator-cxl"
OBJECT_DECLARE_SIMPLE_TYPE(PCIeAccelCXL, PCIE_ACCEL_CXL)

/*
 * CXL.cache D2H Request Types (CXL r3.1 Section 3.2.1)
 * Device-to-Host cache requests for reading host memory cache lines.
 */
typedef enum {
    CXL_CACHE_D2H_REQ_RDCURR      = 0x01,
    CXL_CACHE_D2H_REQ_RDOWN       = 0x02,
    CXL_CACHE_D2H_REQ_RDSHARED    = 0x03,
    CXL_CACHE_D2H_REQ_RDANY       = 0x04,
    CXL_CACHE_D2H_REQ_RDOWNNODATA = 0x05,
} CXLCacheD2HReqType;

/*
 * CXL.cache D2H Data Types (CXL r3.1 Section 3.2.2)
 * Device-to-Host data messages for writing host memory cache lines.
 */
typedef enum {
    CXL_CACHE_D2H_DATA_DIRTYEVICT = 0x01,
    CXL_CACHE_D2H_DATA_CLEANEVICT = 0x02,
    CXL_CACHE_D2H_DATA_WRCURR    = 0x03,
} CXLCacheD2HDataType;

/*
 * CXL.cache line MESI states
 */
typedef enum {
    CXL_CACHE_STATE_INVALID   = 0,
    CXL_CACHE_STATE_SHARED    = 1,
    CXL_CACHE_STATE_EXCLUSIVE = 2,
    CXL_CACHE_STATE_MODIFIED  = 3,
} CXLCacheLineState;

/*
 * CXL.cache line tracking entry.
 *
 * SQEs are 64 bytes = 1 cache line, so each D2H request maps to
 * exactly one cache line.  Tracked for simulation purposes.
 */
typedef struct CXLCacheLine {
    uint64_t host_addr;                 /* Host physical address (64B aligned) */
    uint8_t  data[CXL_CACHE_LINE_SIZE]; /* Cached data */
    CXLCacheLineState state;            /* MESI coherence state */
    bool valid;                         /* Entry is in use */
} CXLCacheLine;

/*
 * CXL.cache protocol state for the accelerator device.
 *
 * Implements a simple direct-mapped cache for host queue entries.
 */
typedef struct CXLCacheState {
    CXLCacheLine *lines;                /* Cache line array */
    uint32_t num_lines;                 /* Number of cache lines */

    /* Statistics */
    uint64_t d2h_reads;
    uint64_t d2h_writes;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t evictions;
} CXLCacheState;

/*
 * PCIeAccelCXL - CXL Type 1 variant of the PCIe accelerator
 *
 * Extends PCIeAccel with CXL component state and CXL.cache
 * protocol tracking.  Registered as TYPE_PCIE_ACCEL_CXL.
 */
struct PCIeAccelCXL {
    PCIeAccel parent_obj;

    /* CXL Component Register State (BAR0) */
    CXLComponentState cxl_cstate;

    /* CXL.cache Protocol State */
    CXLCacheState cache;
    bool cxl_cache_enabled;             /* CXLQCFG.EN: CXL.cache queue mode active */
    bool cxl_cache_error;               /* CXLQCFG.STS.ERR: protocol error occurred */
    bool cxl_cache_last_miss;           /* CXLQCFG.STS.MISS: last access was miss */

    /* Configuration Properties */
    uint32_t cache_lines;               /* Number of cache lines (default 256) */
};

/* CXL.cache-aware DMA operations */
uint16_t accel_cxl_cache_read(PCIeAccelCXL *cxl, uint64_t addr,
                               void *buf, size_t len);
uint16_t accel_cxl_cache_write(PCIeAccelCXL *cxl, uint64_t addr,
                                const void *buf, size_t len);

/* CXL.cache state management */
void cxl_cache_state_init(CXLCacheState *cache, uint32_t num_lines);
void cxl_cache_state_cleanup(CXLCacheState *cache);

#endif /* HW_PCIE_ACCELERATOR_CXL_H */
