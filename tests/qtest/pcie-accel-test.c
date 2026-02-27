/*
 * QTest for the PCIe Accelerator virtual device
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "libqos/qgraph.h"
#include "libqos/pci.h"
#include "hw/misc/pcie-accelerator-regs.h"

#define SQE_SIZE                64
#define CQE_SIZE                16
#define IO_QUEUE_DEPTH          16
#define POLL_TIMEOUT_US         (5 * G_TIME_SPAN_SECOND)
#define DPA_TEST_SIZE           (4 * 1024 * 1024)

typedef struct QAccel {
    QOSGraphObject obj;
    QPCIDevice dev;
    QPCIBar bar0;

    /* Admin queue state */
    uint64_t asq_addr;
    uint64_t acq_addr;
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint8_t  admin_cq_phase;
    uint16_t next_cid;

    /* I/O queue state */
    uint64_t io_sq_addr;
    uint64_t io_cq_addr;
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint8_t  io_cq_phase;
    uint16_t io_qid;
} QAccel;

/* ===== Register access helpers ===== */

static inline uint32_t accel_reg_read(QAccel *a, uint64_t offset)
{
    return qpci_io_readl(&a->dev, a->bar0, offset);
}

static inline void accel_reg_write(QAccel *a, uint64_t offset, uint32_t val)
{
    qpci_io_writel(&a->dev, a->bar0, offset, val);
}

static inline uint64_t accel_reg_read64(QAccel *a, uint64_t offset)
{
    /*
     * Use qtest_readq for a proper 8-byte MMIO access.
     * qpci_io_readq uses qtest_memread which does byte-level reads and
     * may not dispatch through the MMIO handler correctly.
     */
    return qtest_readq(a->dev.bus->qts, a->bar0.addr + offset);
}

static inline void accel_reg_write64(QAccel *a, uint64_t offset, uint64_t val)
{
    qtest_writeq(a->dev.bus->qts, a->bar0.addr + offset, val);
}

/* ===== Command submission ===== */

/*
 * Submit a command on a queue and poll for completion.
 * Returns the status code from the CQE.
 */
static uint16_t accel_submit_cmd(QAccel *a, QGuestAllocator *alloc,
                                 uint64_t sq_addr, uint64_t cq_addr,
                                 uint16_t *sq_tail, uint16_t *cq_head,
                                 uint8_t *cq_phase, uint16_t q_size,
                                 uint16_t qid,
                                 AccelCmd *cmd, AccelCqe *cqe_out)
{
    QTestState *qts = a->dev.bus->qts;
    uint64_t cqe_addr;
    AccelCqe cqe;
    uint32_t status_raw;
    int64_t end_time;

    /* Assign CID */
    cmd->cid = cpu_to_le16(a->next_cid++);

    /* Write SQE to guest memory */
    qtest_memwrite(qts, sq_addr + (uint64_t)(*sq_tail) * SQE_SIZE,
                   cmd, sizeof(*cmd));

    /* Advance SQ tail */
    *sq_tail = (*sq_tail + 1) % q_size;

    /* Ring SQ doorbell */
    accel_reg_write(a, ACCEL_SQ_DOORBELL(qid, ACCEL_DB_STRIDE), *sq_tail);

    /* Poll CQ for completion */
    cqe_addr = cq_addr + (uint64_t)(*cq_head) * CQE_SIZE;
    end_time = g_get_monotonic_time() + POLL_TIMEOUT_US;

    do {
        qtest_clock_step(qts, 100);
        qtest_memread(qts, cqe_addr, &cqe, sizeof(cqe));
        status_raw = le32_to_cpu(cqe.status);
    } while (ACCEL_CQE_PHASE(status_raw) != *cq_phase &&
             g_get_monotonic_time() < end_time);

    g_assert_cmpuint(ACCEL_CQE_PHASE(status_raw), ==, *cq_phase);

    /* Advance CQ head */
    (*cq_head)++;
    if (*cq_head >= q_size) {
        *cq_head = 0;
        *cq_phase ^= 1;
    }

    /* Ring CQ doorbell */
    accel_reg_write(a, ACCEL_CQ_DOORBELL(qid, ACCEL_DB_STRIDE), *cq_head);

    if (cqe_out) {
        *cqe_out = cqe;
    }

    return ACCEL_CQE_SC(status_raw);
}

static uint16_t accel_submit_admin_cmd(QAccel *a, QGuestAllocator *alloc,
                                       AccelCmd *cmd, AccelCqe *cqe_out)
{
    return accel_submit_cmd(a, alloc,
                            a->asq_addr, a->acq_addr,
                            &a->admin_sq_tail, &a->admin_cq_head,
                            &a->admin_cq_phase,
                            ACCEL_ADMIN_QUEUE_SIZE, 0,
                            cmd, cqe_out);
}

static uint16_t accel_submit_io_cmd(QAccel *a, QGuestAllocator *alloc,
                                    AccelCmd *cmd, AccelCqe *cqe_out)
{
    return accel_submit_cmd(a, alloc,
                            a->io_sq_addr, a->io_cq_addr,
                            &a->io_sq_tail, &a->io_cq_head,
                            &a->io_cq_phase,
                            IO_QUEUE_DEPTH, a->io_qid,
                            cmd, cqe_out);
}

/* ===== Controller lifecycle ===== */

static void accel_init_bars(QAccel *a)
{
    qpci_device_enable(&a->dev);
    a->bar0 = qpci_iomap(&a->dev, 0, NULL);
}

static void accel_enable_controller(QAccel *a, QGuestAllocator *alloc)
{
    QTestState *qts = a->dev.bus->qts;
    uint32_t cc, csts;
    int64_t end_time;

    /* Disable controller first to clear any stale state from previous tests */
    cc = accel_reg_read(a, ACCEL_REG_CC);
    if (cc & (1 << ACCEL_CC_EN_SHIFT)) {
        cc &= ~(1 << ACCEL_CC_EN_SHIFT);
        accel_reg_write(a, ACCEL_REG_CC, cc);
        /* Wait for CSTS.RDY to clear */
        end_time = g_get_monotonic_time() + POLL_TIMEOUT_US;
        do {
            qtest_clock_step(qts, 100);
            csts = accel_reg_read(a, ACCEL_REG_CSTS);
        } while ((csts & (1 << ACCEL_CSTS_RDY_SHIFT)) &&
                 g_get_monotonic_time() < end_time);
    }

    /* Allocate page-aligned admin queue buffers */
    a->asq_addr = guest_alloc(alloc, ACCEL_ADMIN_QUEUE_SIZE * SQE_SIZE);
    a->acq_addr = guest_alloc(alloc, ACCEL_ADMIN_QUEUE_SIZE * CQE_SIZE);

    /* Zero out CQ so phase bits start clean */
    qtest_memset(qts, a->acq_addr, 0, ACCEL_ADMIN_QUEUE_SIZE * CQE_SIZE);

    /* Program ASQ and ACQ */
    accel_reg_write64(a, ACCEL_REG_ASQ, a->asq_addr);
    accel_reg_write64(a, ACCEL_REG_ACQ, a->acq_addr);

    /* Enable: set CC.EN=1 */
    cc = accel_reg_read(a, ACCEL_REG_CC);
    cc |= (1 << ACCEL_CC_EN_SHIFT);
    accel_reg_write(a, ACCEL_REG_CC, cc);

    /* Poll CSTS.RDY */
    end_time = g_get_monotonic_time() + POLL_TIMEOUT_US;
    do {
        qtest_clock_step(qts, 100);
        csts = accel_reg_read(a, ACCEL_REG_CSTS);
    } while (!(csts & (1 << ACCEL_CSTS_RDY_SHIFT)) &&
             g_get_monotonic_time() < end_time);

    g_assert_cmphex(csts & (1 << ACCEL_CSTS_RDY_SHIFT), !=, 0);

    a->admin_sq_tail = 0;
    a->admin_cq_head = 0;
    a->admin_cq_phase = 1;
    a->next_cid = 1;
}

/* ===== Queue management ===== */

static uint16_t accel_create_ioq(QAccel *a, QGuestAllocator *alloc,
                                 uint16_t qid, uint16_t vector)
{
    QTestState *qts = a->dev.bus->qts;
    AccelCmd cmd = {};
    uint16_t status;

    /* Delete existing IOQ if present (ignore errors) */
    memset(&cmd, 0, sizeof(cmd));
    cmd.delete_ioq.opcode = ACCEL_ADM_CMD_DELETE_IOQ;
    cmd.delete_ioq.qid = cpu_to_le16(qid);
    accel_submit_admin_cmd(a, alloc, &cmd, NULL);

    a->io_sq_addr = guest_alloc(alloc, IO_QUEUE_DEPTH * SQE_SIZE);
    a->io_cq_addr = guest_alloc(alloc, IO_QUEUE_DEPTH * CQE_SIZE);
    qtest_memset(qts, a->io_cq_addr, 0, IO_QUEUE_DEPTH * CQE_SIZE);

    memset(&cmd, 0, sizeof(cmd));
    cmd.create_ioq.opcode = ACCEL_ADM_CMD_CREATE_IOQ;
    cmd.create_ioq.qid = cpu_to_le16(qid);
    cmd.create_ioq.irq_vector = cpu_to_le16(vector);
    cmd.create_ioq.sq_base = cpu_to_le64(a->io_sq_addr);
    cmd.create_ioq.cq_base = cpu_to_le64(a->io_cq_addr);

    status = accel_submit_admin_cmd(a, alloc, &cmd, NULL);

    if (status == ACCEL_SC_SUCCESS) {
        a->io_qid = qid;
        a->io_sq_tail = 0;
        a->io_cq_head = 0;
        a->io_cq_phase = 1;
    }

    return status;
}

static uint16_t accel_delete_ioq(QAccel *a, QGuestAllocator *alloc,
                                 uint16_t qid)
{
    AccelCmd cmd = {};

    cmd.delete_ioq.opcode = ACCEL_ADM_CMD_DELETE_IOQ;
    cmd.delete_ioq.qid = cpu_to_le16(qid);

    return accel_submit_admin_cmd(a, alloc, &cmd, NULL);
}

/* ===== Test: Register reads ===== */

static void test_reg_read(void *obj, void *data, QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);

    /* CAP register */
    uint64_t cap = accel_reg_read64(a, ACCEL_REG_CAP);

    g_assert_cmphex(cap & (1 << ACCEL_CAP_P2P_SHIFT), !=, 0);
    g_assert_cmphex(cap & (1 << ACCEL_CAP_PRPL_SHIFT), !=, 0);
    g_assert_cmphex(cap & (1 << ACCEL_CAP_SGL_SHIFT), !=, 0);

    uint32_t sqs = (cap >> ACCEL_CAP_SQS_SHIFT) & ACCEL_CAP_SQS_MASK;
    g_assert_cmpuint(sqs, ==, ACCEL_SQES);

    uint32_t cqs = (cap >> ACCEL_CAP_CQS_SHIFT) & ACCEL_CAP_CQS_MASK;
    g_assert_cmpuint(cqs, ==, ACCEL_CQES);

    uint32_t depth = (cap >> ACCEL_CAP_DEPTH_SHIFT) & ACCEL_CAP_DEPTH_MASK;
    g_assert_cmpuint(depth, ==, 12);

    uint32_t maxq = (cap >> ACCEL_CAP_MAXQ_SHIFT) & ACCEL_CAP_MAXQ_MASK;
    g_assert_cmpuint(maxq, ==, 8);

    /* CMBBAR = 2, CMBSZ = 32MB */
    g_assert_cmpuint(accel_reg_read(a, ACCEL_REG_CMBBAR), ==, 2);
    g_assert_cmpuint(accel_reg_read(a, ACCEL_REG_CMBSZ), ==, ACCEL_CMB_SIZE);

    /* CSTS.RDY = 0 initially */
    uint32_t csts = accel_reg_read(a, ACCEL_REG_CSTS);
    g_assert_cmphex(csts & (1 << ACCEL_CSTS_RDY_SHIFT), ==, 0);

    /* DEVSTAT queue_pairs = 0 */
    uint32_t devstat = accel_reg_read(a, ACCEL_REG_DEVSTAT);
    g_assert_cmpuint((devstat >> ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT) & 0xFF,
                     ==, 0);
}

/* ===== Test: CC register write ===== */

static void test_reg_write_cc(void *obj, void *data, QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);

    uint32_t cc = accel_reg_read(a, ACCEL_REG_CC);

    /* IOSQES=6, IOCQES=4 after reset */
    g_assert_cmpuint((cc >> ACCEL_CC_IOSQES_SHIFT) & ACCEL_CC_IOSQES_MASK,
                     ==, ACCEL_SQES);
    g_assert_cmpuint((cc >> ACCEL_CC_IOCQES_SHIFT) & ACCEL_CC_IOCQES_MASK,
                     ==, ACCEL_CQES);

    /* EN=0 after reset */
    g_assert_cmphex(cc & (1 << ACCEL_CC_EN_SHIFT), ==, 0);

    /* INTCOAL write/readback */
    accel_reg_write(a, ACCEL_REG_INTCOAL, 0x0A05);
    g_assert_cmpuint(accel_reg_read(a, ACCEL_REG_INTCOAL), ==, 0x0A05);
}

/* ===== Test: Controller enable ===== */

static void test_controller_enable(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);

    g_assert_cmphex(accel_reg_read(a, ACCEL_REG_CSTS) &
                    (1 << ACCEL_CSTS_RDY_SHIFT), ==, 0);

    accel_enable_controller(a, alloc);

    g_assert_cmphex(accel_reg_read(a, ACCEL_REG_CSTS) &
                    (1 << ACCEL_CSTS_RDY_SHIFT), !=, 0);
}

/* ===== Test: Controller disable ===== */

static void test_controller_disable(void *obj, void *data,
                                    QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);

    accel_enable_controller(a, alloc);

    /* Disable: clear CC.EN */
    uint32_t cc = accel_reg_read(a, ACCEL_REG_CC);
    cc &= ~(1 << ACCEL_CC_EN_SHIFT);
    accel_reg_write(a, ACCEL_REG_CC, cc);

    g_assert_cmphex(accel_reg_read(a, ACCEL_REG_CSTS) &
                    (1 << ACCEL_CSTS_RDY_SHIFT), ==, 0);
}

/* ===== Test: IDENTIFY admin command ===== */

static void test_admin_identify(void *obj, void *data,
                                QGuestAllocator *alloc)
{
    QAccel *a = obj;
    QTestState *qts = a->dev.bus->qts;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);

    /* Allocate buffer for identify data */
    uint64_t id_buf = guest_alloc(alloc, sizeof(AccelIdData));
    qtest_memset(qts, id_buf, 0, sizeof(AccelIdData));

    AccelCmd cmd = {};
    cmd.opcode = ACCEL_ADM_CMD_IDENTIFY;
    cmd.dbd.prpl.prp1 = cpu_to_le64(id_buf);

    uint16_t status = accel_submit_admin_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);

    /* Read identify data back */
    AccelIdData id;
    qtest_memread(qts, id_buf, &id, sizeof(id));

    g_assert_cmpuint(le32_to_cpu(id.data_len), ==, sizeof(AccelIdData));

    /* DPA memory region should be reported (dpa_memdev is set) */
    g_assert_cmpuint(le32_to_cpu(id.mem_region_count), ==, 1);

    uint64_t desc = le64_to_cpu(id.mem_regions[0].desc);
    uint64_t mr_size = (desc >> ACCEL_MR_SIZE_SHIFT) & ACCEL_MR_SIZE_MASK;
    g_assert_cmpuint(mr_size, ==, DPA_TEST_SIZE);
}

/* ===== Test: CREATE/DELETE IOQ ===== */

static void test_admin_create_delete_ioq(void *obj, void *data,
                                         QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);

    uint16_t status = accel_create_ioq(a, alloc, 1, 1);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);

    /* DEVSTAT should show 1 queue pair */
    uint32_t devstat = accel_reg_read(a, ACCEL_REG_DEVSTAT);
    g_assert_cmpuint((devstat >> ACCEL_DEVSTAT_QUEUE_PAIRS_SHIFT) & 0xFF,
                     ==, 1);

    status = accel_delete_ioq(a, alloc, 1);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);
}

/* ===== Test: LOOPBACK I/O command ===== */

static void test_io_loopback(void *obj, void *data, QGuestAllocator *alloc)
{
    QAccel *a = obj;
    QTestState *qts = a->dev.bus->qts;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);
    g_assert_cmpuint(accel_create_ioq(a, alloc, 1, 1), ==, ACCEL_SC_SUCCESS);

    uint32_t length = 256;
    uint64_t data_buf = guest_alloc(alloc, length);

    /* Write test pattern */
    uint8_t pattern[256];
    for (int i = 0; i < (int)length; i++) {
        pattern[i] = (uint8_t)(i & 0xFF);
    }
    qtest_memwrite(qts, data_buf, pattern, length);

    /* LOOPBACK with no XOR */
    AccelCmd cmd = {};
    cmd.opcode = ACCEL_CMD_LOOPBACK;
    cmd.dbd.prpl.prp1 = cpu_to_le64(data_buf);
    cmd.dw.loopback.length = cpu_to_le32(length);
    cmd.dw.loopback.pattern = 0;

    AccelCqe cqe;
    uint16_t status = accel_submit_io_cmd(a, alloc, &cmd, &cqe);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);
    g_assert_cmpuint(le64_to_cpu(cqe.result), ==, length);

    /* Data should be unchanged */
    uint8_t result[256];
    qtest_memread(qts, data_buf, result, length);
    g_assert_cmpmem(result, length, pattern, length);
}

/* ===== Test: LOOPBACK with XOR pattern ===== */

static void test_io_loopback_xor(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    QAccel *a = obj;
    QTestState *qts = a->dev.bus->qts;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);
    g_assert_cmpuint(accel_create_ioq(a, alloc, 1, 1), ==, ACCEL_SC_SUCCESS);

    uint32_t length = 256;
    uint64_t data_buf = guest_alloc(alloc, length);

    /* Fill with known dwords */
    uint32_t orig[64];
    for (int i = 0; i < 64; i++) {
        orig[i] = cpu_to_le32(0xDEADBEEF);
    }
    qtest_memwrite(qts, data_buf, orig, length);

    /* LOOPBACK with XOR pattern */
    uint32_t xor_pat = 0xA5A5A5A5;
    AccelCmd cmd = {};
    cmd.opcode = ACCEL_CMD_LOOPBACK;
    cmd.dbd.prpl.prp1 = cpu_to_le64(data_buf);
    cmd.dw.loopback.length = cpu_to_le32(length);
    cmd.dw.loopback.pattern = cpu_to_le32(xor_pat);

    uint16_t status = accel_submit_io_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);

    /* Each dword should be 0xDEADBEEF ^ 0xA5A5A5A5 */
    uint32_t result[64];
    qtest_memread(qts, data_buf, result, length);
    for (int i = 0; i < 64; i++) {
        g_assert_cmphex(le32_to_cpu(result[i]), ==, 0xDEADBEEF ^ xor_pat);
    }
}

/* ===== Test: MEM_WRITE then MEM_READ ===== */

static void test_io_mem_write_read(void *obj, void *data,
                                   QGuestAllocator *alloc)
{
    QAccel *a = obj;
    QTestState *qts = a->dev.bus->qts;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);
    g_assert_cmpuint(accel_create_ioq(a, alloc, 1, 1), ==, ACCEL_SC_SUCCESS);

    uint32_t length = 512;
    uint64_t host_buf = guest_alloc(alloc, length);

    /* Prepare write data */
    uint8_t write_data[512];
    for (int i = 0; i < (int)length; i++) {
        write_data[i] = (uint8_t)((i * 7 + 3) & 0xFF);
    }
    qtest_memwrite(qts, host_buf, write_data, length);

    /* MEM_WRITE: host → DPA offset 0 */
    AccelCmd cmd = {};
    cmd.mem_write.opcode = ACCEL_CMD_MEM_WRITE;
    cmd.mem_write.dev_addr = cpu_to_le64(0);
    cmd.mem_write.host_addr = cpu_to_le64(host_buf);
    cmd.mem_write.length = cpu_to_le32(length);

    uint16_t status = accel_submit_io_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);

    /* Clear host buffer */
    qtest_memset(qts, host_buf, 0, length);

    /* MEM_READ: DPA offset 0 → host */
    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_read.opcode = ACCEL_CMD_MEM_READ;
    cmd.mem_read.dev_addr = cpu_to_le64(0);
    cmd.mem_read.host_addr = cpu_to_le64(host_buf);
    cmd.mem_read.length = cpu_to_le32(length);

    status = accel_submit_io_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);

    /* Verify data matches */
    uint8_t read_data[512];
    qtest_memread(qts, host_buf, read_data, length);
    g_assert_cmpmem(read_data, length, write_data, length);
}

/* ===== Test: MEM boundary access ===== */

static void test_io_mem_boundary(void *obj, void *data,
                                 QGuestAllocator *alloc)
{
    QAccel *a = obj;
    QTestState *qts = a->dev.bus->qts;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);
    g_assert_cmpuint(accel_create_ioq(a, alloc, 1, 1), ==, ACCEL_SC_SUCCESS);

    uint32_t length = 16;
    uint64_t host_buf = guest_alloc(alloc, length);
    uint8_t pattern[16];
    for (int i = 0; i < 16; i++) {
        pattern[i] = 0xAB;
    }
    qtest_memwrite(qts, host_buf, pattern, length);

    /* Write at end of DPA (DPA_SIZE - 16) */
    AccelCmd cmd = {};
    cmd.mem_write.opcode = ACCEL_CMD_MEM_WRITE;
    cmd.mem_write.dev_addr = cpu_to_le64(DPA_TEST_SIZE - length);
    cmd.mem_write.host_addr = cpu_to_le64(host_buf);
    cmd.mem_write.length = cpu_to_le32(length);

    uint16_t status = accel_submit_io_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);

    /* Read it back */
    qtest_memset(qts, host_buf, 0, length);
    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_read.opcode = ACCEL_CMD_MEM_READ;
    cmd.mem_read.dev_addr = cpu_to_le64(DPA_TEST_SIZE - length);
    cmd.mem_read.host_addr = cpu_to_le64(host_buf);
    cmd.mem_read.length = cpu_to_le32(length);

    status = accel_submit_io_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_SUCCESS);

    uint8_t result[16];
    qtest_memread(qts, host_buf, result, length);
    g_assert_cmpmem(result, length, pattern, length);

    /* Write beyond DPA → LBA_OUT_OF_RANGE */
    memset(&cmd, 0, sizeof(cmd));
    cmd.mem_write.opcode = ACCEL_CMD_MEM_WRITE;
    cmd.mem_write.dev_addr = cpu_to_le64(DPA_TEST_SIZE);
    cmd.mem_write.host_addr = cpu_to_le64(host_buf);
    cmd.mem_write.length = cpu_to_le32(length);

    status = accel_submit_io_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_LBA_OUT_OF_RANGE);
}

/* ===== Test: Invalid opcode ===== */

static void test_error_invalid_opcode(void *obj, void *data,
                                      QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);
    g_assert_cmpuint(accel_create_ioq(a, alloc, 1, 1), ==, ACCEL_SC_SUCCESS);

    AccelCmd cmd = {};
    cmd.opcode = 0xFF;
    cmd.dbd.prpl.prp1 = cpu_to_le64(0x1000);

    uint16_t status = accel_submit_io_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_INVALID_OPCODE);
}

/* ===== Test: Invalid queue ID ===== */

static void test_error_invalid_queue_id(void *obj, void *data,
                                        QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);

    /* Delete admin queue (qid=0) - not allowed */
    g_assert_cmpuint(accel_delete_ioq(a, alloc, 0), ==,
                     ACCEL_SC_INVALID_QUEUE_ID);

    /* Delete non-existent queue (use high qid to avoid stale state) */
    g_assert_cmpuint(accel_delete_ioq(a, alloc, 99), ==,
                     ACCEL_SC_INVALID_QUEUE_ID);
}

/* ===== Test: Queue already exists ===== */

static void test_error_queue_already_exists(void *obj, void *data,
                                            QGuestAllocator *alloc)
{
    QAccel *a = obj;
    accel_init_bars(a);
    accel_enable_controller(a, alloc);

    g_assert_cmpuint(accel_create_ioq(a, alloc, 1, 1), ==, ACCEL_SC_SUCCESS);

    /* Create same qid again */
    AccelCmd cmd = {};
    cmd.create_ioq.opcode = ACCEL_ADM_CMD_CREATE_IOQ;
    cmd.create_ioq.qid = cpu_to_le16(1);
    cmd.create_ioq.irq_vector = cpu_to_le16(1);
    uint64_t dup_sq = guest_alloc(alloc, IO_QUEUE_DEPTH * SQE_SIZE);
    uint64_t dup_cq = guest_alloc(alloc, IO_QUEUE_DEPTH * CQE_SIZE);
    cmd.create_ioq.sq_base = cpu_to_le64(dup_sq);
    cmd.create_ioq.cq_base = cpu_to_le64(dup_cq);

    uint16_t status = accel_submit_admin_cmd(a, alloc, &cmd, NULL);
    g_assert_cmpuint(status, ==, ACCEL_SC_QUEUE_ALREADY_EXISTS);
}

/* ===== QGraph registration ===== */

static void *accel_get_driver(void *obj, const char *interface)
{
    QAccel *a = obj;
    if (!g_strcmp0(interface, "pci-device")) {
        return &a->dev;
    }
    g_assert_not_reached();
}

static void *accel_create(void *pci_bus, QGuestAllocator *alloc, void *addr)
{
    QAccel *a = g_new0(QAccel, 1);
    QPCIBus *bus = pci_bus;

    qpci_device_init(&a->dev, bus, addr);
    a->obj.get_driver = accel_get_driver;

    return &a->obj;
}

static void pcie_accel_register_nodes(void)
{
    QOSGraphEdgeOptions opts = {
        .extra_device_opts = "addr=04.0,max_ioqpairs=16",
        .before_cmd_line =
            "-object memory-backend-ram,id=dpa0,size=4M",
    };

    add_qpci_address(&opts, &(QPCIAddress){ .devfn = QPCI_DEVFN(4, 0) });

    qos_node_create_driver("pcie-accelerator", accel_create);
    qos_node_consumes("pcie-accelerator", "pci-bus", &opts);
    qos_node_produces("pcie-accelerator", "pci-device");

    /* Register tests */
    qos_add_test("reg-read", "pcie-accelerator", test_reg_read, NULL);
    qos_add_test("reg-write-cc", "pcie-accelerator", test_reg_write_cc, NULL);
    qos_add_test("controller-enable", "pcie-accelerator",
                 test_controller_enable, NULL);
    qos_add_test("controller-disable", "pcie-accelerator",
                 test_controller_disable, NULL);

    /* Tests that need DPA memory */
    QOSGraphTestOptions dpa_opts = {
        .edge.extra_device_opts = "dpa_memdev=dpa0",
    };

    qos_add_test("admin-identify", "pcie-accelerator",
                 test_admin_identify, &dpa_opts);
    qos_add_test("admin-create-delete-ioq", "pcie-accelerator",
                 test_admin_create_delete_ioq, &dpa_opts);
    qos_add_test("io-loopback", "pcie-accelerator",
                 test_io_loopback, &dpa_opts);
    qos_add_test("io-loopback-xor", "pcie-accelerator",
                 test_io_loopback_xor, &dpa_opts);
    qos_add_test("io-mem-write-read", "pcie-accelerator",
                 test_io_mem_write_read, &dpa_opts);
    qos_add_test("io-mem-boundary", "pcie-accelerator",
                 test_io_mem_boundary, &dpa_opts);
    qos_add_test("error-invalid-opcode", "pcie-accelerator",
                 test_error_invalid_opcode, &dpa_opts);
    qos_add_test("error-invalid-queue-id", "pcie-accelerator",
                 test_error_invalid_queue_id, &dpa_opts);
    qos_add_test("error-queue-already-exists", "pcie-accelerator",
                 test_error_queue_already_exists, &dpa_opts);
}

libqos_init(pcie_accel_register_nodes);
