/*
 * QEMU PCIe Accelerator - CXL Type 1 Memory Expander Integration
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 * This file implements CXL Type 1 memory expander functionality:
 * - CXL DVSEC (Designated Vendor-Specific Extended Capability) setup
 * - Component registers (HDM decoders, RAS, etc.)
 * - Memory-mapped CXL memory access
 * - Basic CXL.mem protocol support
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/pci/pci.h"
#include "hw/pci/pcie.h"
#include "system/hostmem.h"
#include "system/memory.h"
#include "system/address-spaces.h"

#include "hw/misc/pcie-accelerator.h"
#include "hw/misc/pcie-accelerator-regs.h"

/*
 * CXL DVSEC (Designated Vendor-Specific Extended Capability) IDs
 * From CXL specification r3.1
 */
#define CXL_VENDOR_ID                   0x1E98  /* CXL vendor ID */

#define CXL_DVSEC_PCIE_DEVICE           0x00    /* PCIe DVSEC for CXL Device */
#define CXL_DVSEC_REG_LOCATOR           0x08    /* Register Locator DVSEC */
#define CXL_DVSEC_GPF_DEVICE            0x05    /* GPF Device DVSEC */
#define CXL_DVSEC_FLEX_BUS_PORT         0x07    /* Flex Bus Port DVSEC */

/* CXL Component Register Block IDs */
#define CXL_COMP_REG_BLOCK_SIZE         0x10000  /* 64KB per block */

/*
 * ===== CXL Component Register Operations =====
 */

/**
 * accel_cxl_comp_read - Read from CXL component registers
 * @opaque: Device state
 * @addr: Register offset
 * @size: Access size
 *
 * Returns: Register value
 */
static uint64_t accel_cxl_comp_read(void *opaque, hwaddr addr, unsigned size)
{
    PCIeAccel *n = PCIE_ACCEL(opaque);
    uint64_t val = 0;

    /*
     * Simplified CXL component register implementation.
     * In a full implementation, this would include:
     * - HDM (Host Data Movement) Decoder registers
     * - RAS (Reliability, Availability, Serviceability) capability
     * - Link capability and status
     * - Protocol error handling
     */

    if (addr < 0x1000) {
        /* CXL capability header */
        switch (addr) {
        case 0x00:  /* CXL capability header */
            val = 0x01;  /* Version 1 */
            break;
        case 0x04:  /* CXL capability */
            val = 0x0001;  /* Cache and Memory capable */
            break;
        case 0x08:  /* CXL control */
            val = n->cxl.enabled ? 0x0001 : 0x0000;
            break;
        case 0x0C:  /* CXL status */
            val = 0x0000;  /* No errors */
            break;
        default:
            break;
        }
    } else if (addr >= 0x1000 && addr < 0x2000) {
        /* HDM Decoder Capability */
        switch (addr - 0x1000) {
        case 0x00:  /* HDM Decoder Capability */
            val = 0x00000001;  /* 1 decoder, Type 3 */
            break;
        case 0x04:  /* HDM Decoder Global Control */
            val = 0x00000001;  /* Decoder enabled */
            break;
        case 0x10:  /* HDM Decoder 0 Base Low */
            val = 0;  /* Would be set by host */
            break;
        case 0x14:  /* HDM Decoder 0 Base High */
            val = 0;
            break;
        case 0x18:  /* HDM Decoder 0 Size Low */
            val = n->cxl.size & 0xFFFFFFFF;
            break;
        case 0x1C:  /* HDM Decoder 0 Size High */
            val = (n->cxl.size >> 32) & 0xFFFFFFFF;
            break;
        default:
            break;
        }
    }

    return val;
}

/**
 * accel_cxl_comp_write - Write to CXL component registers
 * @opaque: Device state
 * @addr: Register offset
 * @data: Value to write
 * @size: Access size
 */
static void accel_cxl_comp_write(void *opaque, hwaddr addr, uint64_t data,
                                  unsigned size)
{
    PCIeAccel *n = PCIE_ACCEL(opaque);
    (void)n;  /* Reserved for future HDM decoder implementation */

    /*
     * Simplified implementation. In a full implementation:
     * - HDM decoder configuration
     * - Error injection/handling
     * - Link control
     */

    if (addr >= 0x1000 && addr < 0x2000) {
        /* HDM Decoder writes would be handled here */
        switch (addr - 0x1000) {
        case 0x04:  /* HDM Decoder Global Control */
            /* Would enable/disable HDM decoder */
            break;
        default:
            break;
        }
    }
}

static const MemoryRegionOps accel_cxl_comp_ops = {
    .read = accel_cxl_comp_read,
    .write = accel_cxl_comp_write,
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
 * ===== CXL DVSEC Setup =====
 */

/**
 * accel_cxl_add_dvsec - Add CXL DVSEC capability
 * @pci: PCI device
 * @offset: PCI config space offset
 * @dvsec_id: DVSEC ID
 * @dvsec_rev: DVSEC revision
 * @length: DVSEC length
 *
 * Adds a CXL DVSEC to the PCIe extended capability list.
 *
 * Returns: Offset of next capability, or 0 on error
 */
static uint16_t accel_cxl_add_dvsec(PCIDevice *pci, uint16_t offset,
                                     uint16_t dvsec_id, uint8_t dvsec_rev,
                                     uint16_t length)
{
    uint8_t *config = pci->config;
    uint8_t *wmask = pci->wmask;

    /* DVSEC Header */
    pcie_add_capability(pci, PCI_EXT_CAP_ID_DVSEC, 1, offset, length);

    /* DVSEC Vendor ID */
    pci_set_word(config + offset + 4, CXL_VENDOR_ID);

    /* DVSEC Revision and Length */
    pci_set_byte(config + offset + 6, dvsec_rev);
    pci_set_word(config + offset + 8, dvsec_id);

    /* Make DVSEC read-only */
    memset(wmask + offset, 0, length);

    return offset + length;
}

/**
 * accel_cxl_build_dvsecs - Build all required CXL DVSECs
 * @n: Device state
 *
 * Constructs the CXL DVSEC chain required for CXL Type 1 device.
 */
static void accel_cxl_build_dvsecs(PCIeAccel *n)
{
    PCIDevice *pci = PCI_DEVICE(n);
    uint16_t offset = 0x200;  /* Start DVSECs at offset 0x200 */

    /*
     * DVSEC 0: PCIe DVSEC for CXL Device
     * Describes device CXL capabilities
     */
    offset = accel_cxl_add_dvsec(pci, offset, CXL_DVSEC_PCIE_DEVICE, 1, 0x3C);

    /* Set CXL capability bits */
    pci_set_word(pci->config + offset - 0x3C + 0x0A, 0x0001);  /* Cache.mem capable */

    /*
     * DVSEC 8: Register Locator DVSEC
     * Maps register blocks to BARs
     */
    offset = accel_cxl_add_dvsec(pci, offset, CXL_DVSEC_REG_LOCATOR, 1, 0x24);

    /* Register Block 0: CXL Component Registers in BAR2 */
    uint8_t *reg_loc = pci->config + offset - 0x24 + 0x0C;
    reg_loc[0] = 2;  /* BAR indicator: BAR2 */
    reg_loc[1] = 0;  /* Reserved */
    reg_loc[2] = 0;  /* Block ID: Component registers */
    reg_loc[3] = 0;  /* Reserved */
    /* Offset: 0x0000 */
    pci_set_long(reg_loc + 4, 0x00000000);

    /*
     * DVSEC 5: GPF (Green Power Feature) Device DVSEC
     * Optional power management
     */
    offset = accel_cxl_add_dvsec(pci, offset, CXL_DVSEC_GPF_DEVICE, 1, 0x10);

    /*
     * DVSEC 7: Flex Bus Port DVSEC
     * Describes flex bus configuration
     */
    offset = accel_cxl_add_dvsec(pci, offset, CXL_DVSEC_FLEX_BUS_PORT, 1, 0x20);
}

/*
 * ===== CXL Memory Access Commands =====
 */

/**
 * accel_cmd_cxl_read - Read from CXL memory
 * @n: Device state
 * @req: Request structure
 *
 * Reads data from CXL memory and copies to host memory.
 *
 * Command parameters:
 * - prp1: Destination address in host memory
 * - dw.cxl.dpa: Device Physical Address in CXL memory
 * - dw.cxl.length: Transfer length in bytes
 *
 * Returns: Status code
 */
uint16_t accel_cmd_cxl_read(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint64_t host_addr = le64_to_cpu(cmd->prp1);
    uint64_t cxl_dpa = le64_to_cpu(cmd->dw.cxl.dpa);
    uint32_t length = le32_to_cpu(cmd->dw.cxl.length);
    uint16_t status;
    void *buf;

    if (!n->cxl.enabled) {
        return ACCEL_SC_CXL_NOT_ENABLED;
    }

    /* Validate length */
    if (length == 0 || length > (1 * MiB)) {
        return ACCEL_SC_INVALID_FIELD;
    }

    /* Validate DPA (overflow-safe check) */
    if (cxl_dpa >= n->cxl.size || length > n->cxl.size - cxl_dpa) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: CXL DPA 0x%" PRIx64 " len %u exceeds size %"
                      PRIu64 "\n", cxl_dpa, length, n->cxl.size);
        return ACCEL_SC_CXL_ADDR_INVALID;
    }

    /* Allocate bounce buffer */
    buf = g_malloc(length);
    if (!buf) {
        return ACCEL_SC_INTERNAL_ERROR;
    }

    /* Read from CXL memory */
    if (address_space_read(&n->cxl.as, cxl_dpa, MEMTXATTRS_UNSPECIFIED,
                           buf, length) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Failed to read from CXL memory at DPA 0x%"
                      PRIx64 "\n", cxl_dpa);
        g_free(buf);
        return ACCEL_SC_CXL_ACCESS_ERROR;
    }

    /* Write to host memory */
    status = accel_dma_write_safe(n, host_addr, buf, length);
    g_free(buf);

    if (status == ACCEL_SC_SUCCESS) {
        req->cqe.result = cpu_to_le32(length);
    }

    return status;
}

/**
 * accel_cmd_cxl_write - Write to CXL memory
 * @n: Device state
 * @req: Request structure
 *
 * Reads data from host memory and writes to CXL memory.
 *
 * Command parameters:
 * - prp1: Source address in host memory
 * - dw.cxl.dpa: Device Physical Address in CXL memory
 * - dw.cxl.length: Transfer length in bytes
 *
 * Returns: Status code
 */
uint16_t accel_cmd_cxl_write(PCIeAccel *n, AccelRequest *req)
{
    AccelCmd *cmd = &req->cmd;
    uint64_t host_addr = le64_to_cpu(cmd->prp1);
    uint64_t cxl_dpa = le64_to_cpu(cmd->dw.cxl.dpa);
    uint32_t length = le32_to_cpu(cmd->dw.cxl.length);
    uint16_t status;
    void *buf;

    if (!n->cxl.enabled) {
        return ACCEL_SC_CXL_NOT_ENABLED;
    }

    /* Validate length */
    if (length == 0 || length > (1 * MiB)) {
        return ACCEL_SC_INVALID_FIELD;
    }

    /* Validate DPA (overflow-safe check) */
    if (cxl_dpa >= n->cxl.size || length > n->cxl.size - cxl_dpa) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: CXL DPA 0x%" PRIx64 " len %u exceeds size %"
                      PRIu64 "\n", cxl_dpa, length, n->cxl.size);
        return ACCEL_SC_CXL_ADDR_INVALID;
    }

    /* Allocate bounce buffer */
    buf = g_malloc(length);
    if (!buf) {
        return ACCEL_SC_INTERNAL_ERROR;
    }

    /* Read from host memory */
    status = accel_dma_read_safe(n, host_addr, buf, length);
    if (status != ACCEL_SC_SUCCESS) {
        g_free(buf);
        return status;
    }

    /* Write to CXL memory */
    if (address_space_write(&n->cxl.as, cxl_dpa, MEMTXATTRS_UNSPECIFIED,
                            buf, length) != MEMTX_OK) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "pcie-accel: Failed to write to CXL memory at DPA 0x%"
                      PRIx64 "\n", cxl_dpa);
        g_free(buf);
        return ACCEL_SC_CXL_ACCESS_ERROR;
    }

    g_free(buf);
    req->cqe.result = cpu_to_le32(length);

    return ACCEL_SC_SUCCESS;
}

/*
 * ===== CXL Initialization and Cleanup =====
 */

/**
 * pcie_accel_cxl_init - Initialize CXL Type 1 memory expander
 * @n: Device state
 * @errp: Error pointer
 *
 * Sets up CXL DVSECs, component registers, and memory backing.
 */
void pcie_accel_cxl_init(PCIeAccel *n, Error **errp)
{
    PCIDevice *pci = PCI_DEVICE(n);
    MemoryRegion *mr;

    if (!n->cxl.hostmem) {
        error_setg(errp, "CXL memory backend not configured");
        return;
    }

    /* Get memory region from backend */
    mr = host_memory_backend_get_memory(n->cxl.hostmem);
    if (!mr) {
        error_setg(errp, "Failed to get memory from backend");
        return;
    }

    n->cxl.size = memory_region_size(mr);

    /* Initialize CXL memory address space */
    address_space_init(&n->cxl.as, mr, "pcie-accel-cxl-mem");

    /*
     * Initialize CXL component registers on BAR5.
     * BAR layout:
     *   BAR0: Controller registers
     *   BAR2: P2P scratchpad RAM
     *   BAR4: MSI-X (32-bit)
     *   BAR5: CXL component registers (32-bit)
     */
    memory_region_init_io(&n->bar5_cxl, OBJECT(n), &accel_cxl_comp_ops, n,
                          "pcie-accel-cxl-comp", 64 * KiB);
    pci_register_bar(pci, 5,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_32,
                     &n->bar5_cxl);

    /* Build CXL DVSECs */
    accel_cxl_build_dvsecs(n);

    /* Mark CXL as enabled */
    n->cxl.enabled = true;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "pcie-accel: CXL Type 1 memory expander initialized: %"
                  PRIu64 " MB\n", n->cxl.size / MiB);
}

/**
 * pcie_accel_cxl_exit - Cleanup CXL resources
 * @n: Device state
 */
void pcie_accel_cxl_exit(PCIeAccel *n)
{
    if (!n->cxl.enabled) {
        return;
    }

    /* Cleanup address space */
    address_space_destroy(&n->cxl.as);

    n->cxl.enabled = false;
}
