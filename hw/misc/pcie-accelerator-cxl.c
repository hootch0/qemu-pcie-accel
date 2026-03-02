/*
 * QEMU PCIe Accelerator Device - CXL Type 1 Implementation
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * CXL Type 1 (CXL.io + CXL.cache) variant of the PCIe Accelerator.
 * Uses CXL.cache D2H protocol for host queue fetching instead of
 * standard PCIe DMA.  SQEs are 64 bytes = 1 cache line.
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "hw/pci/pcie_aer.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "qom/object.h"
#include "system/hostmem.h"

#include "hw/misc/pcie-accelerator-cxl.h"
#include "hw/misc/pcie-accelerator-regs.h"

/* Forward declarations */
static const MemoryRegionOps accel_cxl_mmio_ops;

/*
 * ===== CXL.cache State Management =====
 */

void cxl_cache_state_init(CXLCacheState *cache, uint32_t num_lines)
{
    cache->num_lines = num_lines;
    cache->lines = g_new0(CXLCacheLine, num_lines);
    cache->d2h_reads = 0;
    cache->d2h_writes = 0;
    cache->cache_hits = 0;
    cache->cache_misses = 0;
    cache->evictions = 0;
}

void cxl_cache_state_cleanup(CXLCacheState *cache)
{
    g_free(cache->lines);
    cache->lines = NULL;
    cache->num_lines = 0;
}

/*
 * Direct-mapped cache lookup.  Index is derived from the host
 * physical address: (addr / 64) % num_lines.
 */
static CXLCacheLine *cxl_cache_lookup(CXLCacheState *cache, uint64_t addr)
{
    uint32_t index;

    if (!cache->lines || cache->num_lines == 0) {
        return NULL;
    }

    index = (addr / CXL_CACHE_LINE_SIZE) % cache->num_lines;
    CXLCacheLine *line = &cache->lines[index];

    if (line->valid && line->host_addr == addr) {
        return line;
    }
    return NULL;
}

/*
 * Install a cache line.  Evicts the existing entry if the slot
 * is occupied by a different address.
 */
static CXLCacheLine *cxl_cache_install(CXLCacheState *cache, uint64_t addr,
                                        const uint8_t *data,
                                        CXLCacheLineState state)
{
    uint32_t index;
    CXLCacheLine *line;

    if (!cache->lines || cache->num_lines == 0) {
        return NULL;
    }

    index = (addr / CXL_CACHE_LINE_SIZE) % cache->num_lines;
    line = &cache->lines[index];

    if (line->valid && line->host_addr != addr) {
        cache->evictions++;
        qemu_log_mask(LOG_UNIMP,
                      "cxl-cache: evict addr=0x%" PRIx64 " state=%d\n",
                      line->host_addr, line->state);
    }

    line->host_addr = addr;
    line->state = state;
    line->valid = true;
    memcpy(line->data, data, CXL_CACHE_LINE_SIZE);

    return line;
}

/*
 * ===== CXL.cache D2H Protocol Operations =====
 */

/**
 * accel_cxl_cache_read - D2H Read: fetch cache line(s) from host memory
 *
 * Simulates CXL.cache D2H RdOwn requests.  On cache hit, returns
 * cached data.  On miss, issues a DMA read and installs the line.
 * Operates at 64-byte cache-line granularity.
 */
uint16_t accel_cxl_cache_read(PCIeAccelCXL *cxl, uint64_t addr,
                               void *buf, size_t len)
{
    PCIeAccel *n = PCIE_ACCEL(cxl);
    CXLCacheState *cache = &cxl->cache;
    uint64_t line_addr;
    uint16_t status;
    uint64_t offset;

    for (offset = 0; offset < len; offset += CXL_CACHE_LINE_SIZE) {
        uint32_t chunk = MIN(len - offset, CXL_CACHE_LINE_SIZE);
        line_addr = (addr + offset) & ~(uint64_t)(CXL_CACHE_LINE_SIZE - 1);
        uint32_t line_offset = (addr + offset) & (CXL_CACHE_LINE_SIZE - 1);

        /* Check cache for hit */
        CXLCacheLine *line = cxl_cache_lookup(cache, line_addr);
        if (line && line->state != CXL_CACHE_STATE_INVALID) {
            memcpy((uint8_t *)buf + offset, line->data + line_offset, chunk);
            cache->cache_hits++;
            cxl->cxl_cache_last_miss = false;
            qemu_log_mask(LOG_UNIMP,
                          "cxl-cache: D2H Read HIT addr=0x%" PRIx64
                          " state=%d\n", line_addr, line->state);
            continue;
        }

        /* Cache miss -- D2H RdOwn to host */
        cache->cache_misses++;
        cache->d2h_reads++;
        cxl->cxl_cache_last_miss = true;

        qemu_log_mask(LOG_UNIMP,
                      "cxl-cache: D2H RdOwn addr=0x%" PRIx64 "\n", line_addr);

        /* Fetch full cache line from host memory */
        uint8_t line_buf[CXL_CACHE_LINE_SIZE];
        status = accel_dma_read_safe(n, line_addr, line_buf,
                                     CXL_CACHE_LINE_SIZE);
        if (status != ACCEL_SC_SUCCESS) {
            cxl->cxl_cache_error = true;
            return status;
        }

        /* Install in cache */
        cxl_cache_install(cache, line_addr, line_buf,
                          CXL_CACHE_STATE_EXCLUSIVE);

        /* Copy requested portion to caller */
        memcpy((uint8_t *)buf + offset, line_buf + line_offset, chunk);
    }

    return ACCEL_SC_SUCCESS;
}

/**
 * accel_cxl_cache_write - D2H Write: write cache line(s) to host memory
 *
 * Simulates CXL.cache D2H WrCurr/DirtyEvict.  Writes data through
 * to host memory and invalidates the local cache line.
 * For sub-cache-line writes (e.g. 16-byte CQEs), performs
 * read-modify-write at cache-line granularity.
 */
uint16_t accel_cxl_cache_write(PCIeAccelCXL *cxl, uint64_t addr,
                                const void *buf, size_t len)
{
    PCIeAccel *n = PCIE_ACCEL(cxl);
    CXLCacheState *cache = &cxl->cache;
    uint16_t status;
    uint64_t offset;

    for (offset = 0; offset < len; offset += CXL_CACHE_LINE_SIZE) {
        uint64_t cur_addr = addr + offset;
        uint64_t line_addr = cur_addr & ~(uint64_t)(CXL_CACHE_LINE_SIZE - 1);
        uint32_t line_offset = cur_addr & (CXL_CACHE_LINE_SIZE - 1);
        uint32_t chunk = MIN(len - offset, CXL_CACHE_LINE_SIZE - line_offset);

        cache->d2h_writes++;

        if (chunk < CXL_CACHE_LINE_SIZE) {
            /*
             * Sub-cache-line write (e.g. 16-byte CQE).
             * Read-modify-write: fetch the full line, patch in
             * the new data, then write the full line back.
             */
            uint8_t line_buf[CXL_CACHE_LINE_SIZE];

            /* Try cache first */
            CXLCacheLine *line = cxl_cache_lookup(cache, line_addr);
            if (line && line->state != CXL_CACHE_STATE_INVALID) {
                memcpy(line_buf, line->data, CXL_CACHE_LINE_SIZE);
                cache->cache_hits++;
            } else {
                cache->cache_misses++;
                status = accel_dma_read_safe(n, line_addr, line_buf,
                                             CXL_CACHE_LINE_SIZE);
                if (status != ACCEL_SC_SUCCESS) {
                    cxl->cxl_cache_error = true;
                    return status;
                }
            }

            /* Patch in the new data */
            memcpy(line_buf + line_offset, (const uint8_t *)buf + offset,
                   chunk);

            qemu_log_mask(LOG_UNIMP,
                          "cxl-cache: D2H WrCurr addr=0x%" PRIx64
                          " (RMW, %u bytes at offset %u)\n",
                          line_addr, chunk, line_offset);

            /* Write full line back to host */
            status = accel_dma_write_safe(n, line_addr, line_buf,
                                          CXL_CACHE_LINE_SIZE);
            if (status != ACCEL_SC_SUCCESS) {
                cxl->cxl_cache_error = true;
                return status;
            }

            /* Invalidate cache line after write-through */
            if (line) {
                line->state = CXL_CACHE_STATE_INVALID;
                line->valid = false;
                cache->evictions++;
            }
        } else {
            /* Full cache-line write */
            qemu_log_mask(LOG_UNIMP,
                          "cxl-cache: D2H WrCurr addr=0x%" PRIx64 "\n",
                          line_addr);

            status = accel_dma_write_safe(n, line_addr,
                                          (const uint8_t *)buf + offset,
                                          CXL_CACHE_LINE_SIZE);
            if (status != ACCEL_SC_SUCCESS) {
                return status;
            }

            /* Invalidate cache line */
            CXLCacheLine *line = cxl_cache_lookup(cache, line_addr);
            if (line) {
                line->state = CXL_CACHE_STATE_INVALID;
                line->valid = false;
                cache->evictions++;
            }
        }
    }

    return ACCEL_SC_SUCCESS;
}

/*
 * DMA ops wrappers matching the PCIeAccel dma_ops signature.
 * These cast the PCIeAccel* to PCIeAccelCXL* and delegate to
 * the CXL.cache protocol functions.
 */
static uint16_t accel_cxl_dma_read(PCIeAccel *n, uint64_t addr,
                                    void *buf, size_t len)
{
    PCIeAccelCXL *cxl = PCIE_ACCEL_CXL(n);
    return accel_cxl_cache_read(cxl, addr, buf, len);
}

static uint16_t accel_cxl_dma_write(PCIeAccel *n, uint64_t addr,
                                     const void *buf, size_t len)
{
    PCIeAccelCXL *cxl = PCIE_ACCEL_CXL(n);
    return accel_cxl_cache_write(cxl, addr, buf, len);
}

/*
 * ===== CXL DVSEC Setup =====
 */

static void accel_cxl_build_dvsecs(PCIeAccelCXL *s)
{
    CXLComponentState *cxl_cstate = &s->cxl_cstate;
    uint8_t *dvsec;

    /*
     * CXL Device DVSEC (ID 0) - CXL r3.1 Section 8.1.3
     *
     * For Type 1 (CXL.io + CXL.cache):
     *   cap bit 1: CXL.cache capable = 1
     *   cap bit 2: CXL.mem capable = 0
     *   cap bit 4: CXL.io capable = 1
     *
     * cap = 0x12: CXL.cache + CXL.io, no CXL.mem
     */
    dvsec = (uint8_t *)&(CXLDVSECDevice){
        .cap = 0x12,
        .ctrl = 0x02,
        .status2 = 0x2,
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE1_DEVICE,
                               PCIE_CXL_DEVICE_DVSEC_LENGTH,
                               PCIE_CXL_DEVICE_DVSEC,
                               PCIE_CXL31_DEVICE_DVSEC_REVID, dvsec);

    /*
     * Register Locator DVSEC (ID 8) - CXL r3.1 Section 8.1.9
     *
     * Points to CXL Component Register Block in BAR0.
     * Type 1 has no CXL Device Register Block (uses accelerator
     * admin queues instead of CXL mailbox).
     */
    dvsec = (uint8_t *)&(CXLDVSECRegisterLocator){
        .rsvd         = 0,
        .reg0_base_lo = RBI_COMPONENT_REG | CXL_COMPONENT_REG_BAR_IDX,
        .reg0_base_hi = 0,
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE1_DEVICE,
                               REG_LOC_DVSEC_LENGTH, REG_LOC_DVSEC,
                               REG_LOC_DVSEC_REVID, dvsec);

    /*
     * Flexbus Port DVSEC (ID 7) - CXL r3.1 Section 8.1.8
     *
     * cap = 0x22: CXL.cache + CXL.io capable, 68B flit
     */
    dvsec = (uint8_t *)&(CXLDVSECPortFlexBus){
        .cap    = 0x22,
        .ctrl   = 0x02,
        .status = 0x22,
        .rcvd_mod_ts_data_phase1 = 0xef,
    };
    cxl_component_create_dvsec(cxl_cstate, CXL2_TYPE1_DEVICE,
                               PCIE_CXL3_FLEXBUS_PORT_DVSEC_LENGTH,
                               PCIE_FLEXBUS_PORT_DVSEC,
                               PCIE_CXL3_FLEXBUS_PORT_DVSEC_REVID, dvsec);
}

/*
 * ===== Device Lifecycle =====
 */

static void pcie_accel_cxl_realize(PCIDevice *pci_dev, Error **errp)
{
    ERRP_GUARD();
    PCIeAccelCXL *s = PCIE_ACCEL_CXL(pci_dev);
    PCIeAccel *n = PCIE_ACCEL(pci_dev);
    CXLComponentState *cxl_cstate = &s->cxl_cstate;
    ComponentRegisters *regs = &cxl_cstate->crb;
    MemoryRegion *mr = &regs->component_registers;
    Error *local_err = NULL;
    int ret;

    /* Assign sequential device ID */
    static uint16_t next_dev_id;
    n->dev_id = next_dev_id++;

    /* PCIe endpoint capability */
    ret = pcie_endpoint_cap_init(pci_dev, 0x80);
    if (ret < 0) {
        error_setg(errp, "Failed to initialize PCIe endpoint capability");
        return;
    }

    /* AER capability */
    pcie_aer_init(pci_dev, PCI_ERR_VER, 0x100, PCI_ERR_SIZEOF, NULL);

    /* CXL DVSECs (placed after AER in extended config space) */
    cxl_cstate->dvsec_offset = 0x100 + PCI_ERR_SIZEOF;
    cxl_cstate->pdev = pci_dev;
    accel_cxl_build_dvsecs(s);

    /* BAR0: CXL Component Register Block (64KB, 64-bit) */
    cxl_component_register_block_init(OBJECT(pci_dev), cxl_cstate,
                                      TYPE_PCIE_ACCEL_CXL);
    cxl_component_register_init_common(regs->cache_mem_registers,
                                       regs->cache_mem_regs_write_mask,
                                       CXL2_TYPE1_DEVICE);
    pci_register_bar(pci_dev, CXL_COMPONENT_REG_BAR_IDX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, mr);

    /*
     * BAR2: Combined accelerator MMIO + CMB (64-bit, prefetchable)
     *
     * Layout within BAR2:
     *   [0x0000 .. ACCEL_BAR0_SIZE)       Accelerator MMIO registers
     *   [ACCEL_BAR0_SIZE .. +ACCEL_CMB_SIZE)  CMB RAM
     */
    uint64_t bar2_size = ACCEL_BAR0_SIZE + ACCEL_CMB_SIZE;
    /* Round up to power of 2 */
    bar2_size = pow2ceil(bar2_size);

    MemoryRegion *bar2 = g_new0(MemoryRegion, 1);
    memory_region_init(bar2, OBJECT(n), "accel-cxl-bar2", bar2_size);

    /* Accelerator MMIO at offset 0 */
    memory_region_init_io(&n->bar0, OBJECT(n), &accel_cxl_mmio_ops, n,
                          "pcie-accel-mmio", ACCEL_BAR0_SIZE);
    memory_region_add_subregion(bar2, 0, &n->bar0);

    /* CMB RAM at offset ACCEL_BAR0_SIZE */
    memory_region_init_ram(&n->cmb, OBJECT(n), "pcie-accel-cmb",
                           ACCEL_CMB_SIZE, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return;
    }
    memory_region_add_subregion(bar2, ACCEL_BAR0_SIZE, &n->cmb);

    pci_register_bar(pci_dev, CXL_DEVICE_REG_BAR_IDX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64 |
                     PCI_BASE_ADDRESS_MEM_PREFETCH,
                     bar2);

    /* BAR4: MSI-X (32-bit) */
    memory_region_init(&n->msix_bar, OBJECT(n), "pcie-accel-msix",
                       ACCEL_BAR4_SIZE);
    pci_register_bar(pci_dev, 4,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_32,
                     &n->msix_bar);

    ret = msix_init(pci_dev, n->max_ioqpairs + 1,
                    &n->msix_bar, 4, ACCEL_MSIX_TABLE_OFFSET,
                    &n->msix_bar, 4, ACCEL_MSIX_PBA_OFFSET,
                    0x00, &local_err);
    if (ret < 0) {
        error_propagate(errp, local_err);
        return;
    }
    msix_vector_use(pci_dev, 0);

    /* DPA memory backend (optional) */
    if (n->dpa_memdev) {
        n->dpa_mr = host_memory_backend_get_memory(n->dpa_memdev);
        if (!n->dpa_mr) {
            error_setg(errp, "Failed to get DPA memory from backend");
            return;
        }
    }

    /* Initialize capability register (same as base + CXL.cache bit) */
    n->bar.cap = (1ULL << ACCEL_CAP_P2P_SHIFT) |
                 ((uint64_t)n->sva.enabled << ACCEL_CAP_SVA_SHIFT) |
                 (1ULL << ACCEL_CAP_PRPL_SHIFT) |
                 (1ULL << ACCEL_CAP_SGL_SHIFT) |
                 (0x8ULL << ACCEL_CAP_P2P_CH_BS_SHIFT) |
                 ((uint64_t)ACCEL_SQES << ACCEL_CAP_SQS_SHIFT) |
                 ((uint64_t)ACCEL_CQES << ACCEL_CAP_CQS_SHIFT) |
                 (12ULL << ACCEL_CAP_DEPTH_SHIFT) |
                 (8ULL << ACCEL_CAP_MAXQ_SHIFT) |
                 (4ULL << ACCEL_CAP_MAXR_SHIFT) |
                 (1ULL << ACCEL_CAP_CXL_CACHE_SHIFT);

    /* P2P configuration */
    n->bar.p2pcfg = (n->p2p.max_peers << ACCEL_P2PCFG_MAX_DEVICES_SHIFT) |
                    (n->p2p.max_xfers_per_peer << ACCEL_P2PCFG_MAX_XFERS_SHIFT);

    /* CMB registers */
    n->bar.cmbbar = CXL_DEVICE_REG_BAR_IDX;
    n->bar.cmbsz = ACCEL_CMB_SIZE;

    /* P2P ring configuration */
    uint32_t ring_size_4k = ACCEL_RING_SIZE / 4096;
    n->p2p.p2rcfg = (ACCEL_P2R_MAX_SLOTS << ACCEL_P2RCFG_SLOTS_SHIFT) |
                    (ring_size_4k << ACCEL_P2RCFG_RING_SIZE_SHIFT);

    /* Page size */
    n->page_size = 4096;
    n->page_bits = 12;

    /* Queue arrays */
    n->sq = g_new0(AccelSQueue *, n->max_ioqpairs + 1);
    n->cq = g_new0(AccelCQueue *, n->max_ioqpairs + 1);

    /* P2P peer list */
    QTAILQ_INIT(&n->p2p.peer_list);

    /* PASID/SVA */
    if (n->sva.enabled) {
        pcie_pasid_init(pci_dev, 0x150, n->sva.pasid_width, false, false);
        pcie_ats_init(pci_dev, 0x170, false);
        int max_pasid = 1 << n->sva.pasid_width;
        n->sva.pasid_as = g_new0(AddressSpace *, max_pasid);
    }

    /* Initialize CXL.cache state */
    cxl_cache_state_init(&s->cache, s->cache_lines);

    /*
     * Start with CXL.cache disabled (standard PCIe DMA).
     * The driver must write CXLQCFG.EN=1 to activate CXL.cache
     * D2H protocol for queue fetching.
     */
    s->cxl_cache_enabled = false;
    s->cxl_cache_error = false;
    s->cxl_cache_last_miss = false;
    /* dma_ops keep the base PCIe DMA defaults from parent realize */

    /* Reset */
    pcie_accel_reset(DEVICE(n));
}

static void pcie_accel_cxl_exit(PCIDevice *pci_dev)
{
    PCIeAccelCXL *s = PCIE_ACCEL_CXL(pci_dev);

    /* Cleanup CXL.cache state */
    cxl_cache_state_cleanup(&s->cache);

    /* Call parent exit */
    pcie_accel_exit(pci_dev);
}

static void pcie_accel_cxl_reset(DeviceState *dev)
{
    PCIeAccelCXL *s = PCIE_ACCEL_CXL(dev);

    /* Reset CXL.cache -- invalidate all lines and clear stats */
    if (s->cache.lines) {
        memset(s->cache.lines, 0,
               s->cache.num_lines * sizeof(CXLCacheLine));
    }
    s->cache.d2h_reads = 0;
    s->cache.d2h_writes = 0;
    s->cache.cache_hits = 0;
    s->cache.cache_misses = 0;
    s->cache.evictions = 0;

    /* Call parent reset */
    pcie_accel_reset(dev);

    /* Reset CXL.cache queue mode to disabled (standard PCIe DMA) */
    s->cxl_cache_enabled = false;
    s->cxl_cache_error = false;
    s->cxl_cache_last_miss = false;
    /* dma_ops stay as base PCIe DMA after parent reset */
}

/*
 * ===== MMIO Handlers =====
 *
 * The CXL variant reuses the same MMIO register handlers as the base
 * device.  We expose these ops so the realize function can init the
 * MMIO subregion.  The extern declaration references the base device's
 * MMIO ops which are static, so we define thin wrappers here.
 */

static uint64_t accel_cxl_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    /* Delegate to base MMIO read via the public interface */
    PCIeAccel *n = PCIE_ACCEL(opaque);
    uint64_t val = 0;

    if (addr >= ACCEL_REG_DOORBELL) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel-cxl: Read from write-only doorbell: 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }

    switch (addr) {
    case ACCEL_REG_CAP:
        val = n->bar.cap;
        break;
    case ACCEL_REG_CC:
        val = n->bar.cc;
        break;
    case ACCEL_REG_CSTS:
        val = n->bar.csts;
        break;
    case ACCEL_REG_ASQ:
        val = n->bar.asq;
        break;
    case ACCEL_REG_ACQ:
        val = n->bar.acq;
        break;
    case ACCEL_REG_CMBBAR:
        val = n->bar.cmbbar;
        break;
    case ACCEL_REG_CMBSZ:
        val = n->bar.cmbsz;
        break;
    case ACCEL_REG_P2PCFG:
        val = n->bar.p2pcfg;
        break;
    case ACCEL_REG_P2RCFG:
        val = n->p2p.p2rcfg;
        break;
    case ACCEL_REG_INTCOAL:
        val = n->bar.intcoal;
        break;
    case ACCEL_REG_DEVSTAT:
        n->bar.devstat =
            ((n->p2p.num_peers & 0xFF) << ACCEL_DEVSTAT_NUM_PEERS_SHIFT) |
            ((0 & 0xFF) << ACCEL_DEVSTAT_ACTIVE_XFERS_SHIFT) |
            ((n->conf_ioqpairs & 0xFF) << ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT);
        val = n->bar.devstat;
        break;
    case ACCEL_REG_CXLQCFG: {
        PCIeAccelCXL *cxl = PCIE_ACCEL_CXL(opaque);
        val = (cxl->cxl_cache_enabled ? 1 : 0) << ACCEL_CXLQCFG_EN_SHIFT;
        /* FLUSH reads back as 0 */
        /* Status bits */
        if (cxl->cxl_cache_enabled) {
            val |= (1 << ACCEL_CXLQCFG_ACTIVE_SHIFT);
        }
        if (cxl->cxl_cache_last_miss) {
            val |= (1 << ACCEL_CXLQCFG_MISS_SHIFT);
        }
        if (cxl->cxl_cache_error) {
            val |= (1 << ACCEL_CXLQCFG_ERR_SHIFT);
        }
        /* Cache line count (read-only) */
        val |= ((uint32_t)cxl->cache_lines & ACCEL_CXLQCFG_LINES_MASK)
                    << ACCEL_CXLQCFG_LINES_SHIFT;
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel-cxl: Read unknown reg 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }

    return val;
}

static void accel_cxl_mmio_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    PCIeAccel *n = PCIE_ACCEL(opaque);

    /* Handle P2P ring doorbell writes (0x4000+) */
    if (addr >= ACCEL_P2R_DB_BASE &&
        addr < ACCEL_P2R_DB_BASE + ACCEL_P2R_MAX_SLOTS * ACCEL_P2R_DB_STRIDE) {
        accel_p2p_ring_doorbell(n, addr - ACCEL_P2R_DB_BASE, data);
        return;
    }

    /* Handle host queue doorbell writes (0x1000+) */
    if (addr >= ACCEL_REG_DOORBELL) {
        /* Inline doorbell processing -- same logic as base device */
        hwaddr db_offset = addr - ACCEL_REG_DOORBELL;
        uint32_t db_idx = db_offset / ACCEL_DB_STRIDE;
        bool is_cq = (db_idx & 1);
        uint32_t qid = db_idx / 2;

        if (is_cq) {
            if (accel_check_cqid(n, qid)) {
                AccelCQueue *cq = n->cq[qid];
                uint16_t new_head = data & 0xFFFF;
                if (new_head < cq->size) {
                    cq->head = new_head;
                    if (!QTAILQ_EMPTY(&cq->req_list)) {
                        qemu_bh_schedule(cq->bh);
                    }
                }
            }
        } else {
            if (accel_check_sqid(n, qid)) {
                AccelSQueue *sq = n->sq[qid];
                uint16_t new_tail = data & 0xFFFF;
                if (new_tail < sq->size) {
                    sq->tail = new_tail;
                    qemu_bh_schedule(sq->bh);
                }
            }
        }
        return;
    }

    switch (addr) {
    case ACCEL_REG_CC:
        n->bar.cc = data;
        if (data & (1 << ACCEL_CC_EN_SHIFT)) {
            if (!(n->bar.csts & (1 << ACCEL_CSTS_RDY_SHIFT))) {
                if (n->bar.asq && n->bar.acq) {
                    accel_set_ctrl_ready(n, true);
                }
            }
        } else {
            accel_set_ctrl_ready(n, false);
        }
        break;
    case ACCEL_REG_ASQ:
        if (!(n->bar.csts & (1 << ACCEL_CSTS_RDY_SHIFT))) {
            n->bar.asq = data;
            accel_init_sq(&n->admin_sq, n, data, 0, 0,
                          ACCEL_ADMIN_QUEUE_SIZE);
            n->sq[0] = &n->admin_sq;
        }
        break;
    case ACCEL_REG_ACQ:
        if (!(n->bar.csts & (1 << ACCEL_CSTS_RDY_SHIFT))) {
            n->bar.acq = data;
            accel_init_cq(&n->admin_cq, n, data, 0, 0,
                          ACCEL_ADMIN_QUEUE_SIZE, 1);
            n->cq[0] = &n->admin_cq;
        }
        break;
    case ACCEL_REG_INTCOAL:
        n->bar.intcoal = data;
        n->intcoal_thresh = (data >> ACCEL_INTCOAL_THRESH_SHIFT) & 0xFF;
        n->intcoal_time = (data >> ACCEL_INTCOAL_TIME_SHIFT) & 0xFF;
        break;
    case ACCEL_REG_CXLQCFG: {
        PCIeAccelCXL *cxl = PCIE_ACCEL_CXL(opaque);
        bool new_en = (data >> ACCEL_CXLQCFG_EN_SHIFT) & ACCEL_CXLQCFG_EN_MASK;
        bool flush  = (data >> ACCEL_CXLQCFG_FLUSH_SHIFT) & ACCEL_CXLQCFG_FLUSH_MASK;

        if (new_en && !cxl->cxl_cache_enabled) {
            /*
             * Enable CXL.cache queue mode.
             * SQ fetches (dma_ops.read) use D2H RdOwn.
             * CQ posts (dma_ops.write) use D2H WrCurr.
             */
            cxl->cxl_cache_enabled = true;
            n->dma_ops.read = accel_cxl_dma_read;
            n->dma_ops.write = accel_cxl_dma_write;
            cxl->cxl_cache_error = false;
            qemu_log_mask(LOG_UNIMP,
                          "cxl-cache: Enabled CXL.cache queue mode "
                          "(SQ read: D2H RdOwn, CQ write: D2H WrCurr)\n");
        } else if (!new_en && cxl->cxl_cache_enabled) {
            /*
             * Disable CXL.cache queue mode.
             * Revert to standard PCIe DMA for both SQ and CQ.
             */
            cxl->cxl_cache_enabled = false;
            n->dma_ops.read = accel_dma_read_safe;
            n->dma_ops.write = accel_dma_write_safe;
            qemu_log_mask(LOG_UNIMP,
                          "cxl-cache: Disabled CXL.cache queue mode "
                          "(reverted to PCIe DMA for SQ/CQ)\n");
        }

        /* FLUSH: invalidate all cache lines (only when cache is disabled) */
        if (flush && !cxl->cxl_cache_enabled) {
            if (cxl->cache.lines) {
                memset(cxl->cache.lines, 0,
                       cxl->cache.num_lines * sizeof(CXLCacheLine));
            }
            cxl->cache.d2h_reads = 0;
            cxl->cache.d2h_writes = 0;
            cxl->cache.cache_hits = 0;
            cxl->cache.cache_misses = 0;
            cxl->cache.evictions = 0;
            cxl->cxl_cache_error = false;
            cxl->cxl_cache_last_miss = false;
            qemu_log_mask(LOG_UNIMP,
                          "cxl-cache: Flushed all cache lines\n");
        }
        break;
    }
    case ACCEL_REG_CAP:
    case ACCEL_REG_CSTS:
    case ACCEL_REG_CMBBAR:
    case ACCEL_REG_CMBSZ:
    case ACCEL_REG_P2PCFG:
    case ACCEL_REG_P2RCFG:
    case ACCEL_REG_DEVSTAT:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel-cxl: Write to RO reg 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel-cxl: Write unknown reg 0x%" HWADDR_PRIx "\n",
                      addr);
        break;
    }
}

static const MemoryRegionOps accel_cxl_mmio_ops = {
    .read = accel_cxl_mmio_read,
    .write = accel_cxl_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 8,
    },
};

/*
 * ===== Device Properties and Class Definition =====
 */

static const Property pcie_accel_cxl_props[] = {
    DEFINE_PROP_UINT32("cache_lines", PCIeAccelCXL, cache_lines, 256),
};

static void pcie_accel_cxl_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);

    pc->realize = pcie_accel_cxl_realize;
    pc->exit = pcie_accel_cxl_exit;
    pc->vendor_id = ACCEL_PCIE_VENDOR_ID;
    pc->device_id = 0x5679;
    pc->revision = ACCEL_PCIE_REVISION;
    pc->class_id = ACCEL_PCIE_CLASS;

    device_class_set_props(dc, pcie_accel_cxl_props);
    dc->desc = "PCIe Accelerator Device (CXL Type 1)";
    device_class_set_legacy_reset(dc, pcie_accel_cxl_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo pcie_accel_cxl_info = {
    .name = TYPE_PCIE_ACCEL_CXL,
    .parent = TYPE_PCIE_ACCEL,
    .instance_size = sizeof(PCIeAccelCXL),
    .class_init = pcie_accel_cxl_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CXL_DEVICE },
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void pcie_accel_cxl_register_types(void)
{
    type_register_static(&pcie_accel_cxl_info);
}

type_init(pcie_accel_cxl_register_types)
