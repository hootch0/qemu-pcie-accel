/*
 * QTest for the PCIe Accelerator CXL Type 1 Device
 *
 * Tests CXL Type 1 device instantiation on a CXL bus topology.
 * Verifies the device can be created on CXL root ports with the
 * required DVSECs and CXL interfaces, with and without DPA memory.
 *
 * Queue functionality (admin commands, I/O operations) is tested
 * by the base pcie-accelerator qos-test which exercises the same
 * code paths -- the CXL variant inherits queue handling and overrides
 * only the DMA ops with CXL.cache protocol.
 *
 * Copyright (c) 2026 QEMU Project
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest-single.h"

/*
 * CXL bus topology macros.
 * The accelerator-cxl device must sit behind a CXL root port on a
 * q35 machine with CXL enabled.
 */
#define QEMU_CXL_BASE \
    "-machine q35,cxl=on " \
    "-device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 " \
    "-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G " \
    "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 "

#define QEMU_ACCEL_CXL \
    "-device pcie-accelerator-cxl,bus=rp0,id=accel0 "

#define QEMU_ACCEL_CXL_DPA \
    "-object memory-backend-ram,id=dpa0,size=4M " \
    "-device pcie-accelerator-cxl,bus=rp0,id=accel0,dpa_memdev=dpa0 "

/* ===== Test: Basic instantiation on CXL bus ===== */

static void test_cxl_accel_instantiate(void)
{
    qtest_start(QEMU_CXL_BASE QEMU_ACCEL_CXL);
    qtest_end();
}

/* ===== Test: Instantiation with DPA memory backend ===== */

static void test_cxl_accel_instantiate_dpa(void)
{
    qtest_start(QEMU_CXL_BASE QEMU_ACCEL_CXL_DPA);
    qtest_end();
}

/* ===== Test: Custom cache line count property ===== */

static void test_cxl_accel_cache_lines_prop(void)
{
    qtest_start(
        QEMU_CXL_BASE
        "-device pcie-accelerator-cxl,bus=rp0,id=accel0,cache_lines=512 "
    );
    qtest_end();
}

/* ===== Test: Two CXL accelerators on separate root ports ===== */

static void test_cxl_accel_dual_devices(void)
{
    qtest_start(
        "-machine q35,cxl=on "
        "-device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 "
        "-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G "
        "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 "
        "-device cxl-rp,id=rp1,bus=cxl.0,chassis=0,slot=1 "
        "-device pcie-accelerator-cxl,bus=rp0,id=accel0 "
        "-device pcie-accelerator-cxl,bus=rp1,id=accel1 "
    );
    qtest_end();
}

/* ===== Test: CXL accel with DPA on dual root ports ===== */

static void test_cxl_accel_dual_with_dpa(void)
{
    qtest_start(
        "-machine q35,cxl=on "
        "-device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 "
        "-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G "
        "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 "
        "-device cxl-rp,id=rp1,bus=cxl.0,chassis=0,slot=1 "
        "-object memory-backend-ram,id=dpa0,size=4M "
        "-object memory-backend-ram,id=dpa1,size=4M "
        "-device pcie-accelerator-cxl,bus=rp0,id=accel0,dpa_memdev=dpa0 "
        "-device pcie-accelerator-cxl,bus=rp1,id=accel1,dpa_memdev=dpa1 "
    );
    qtest_end();
}

/* ===== Test: CXL accel alongside CXL Type 3 device ===== */

static void test_cxl_accel_with_type3(void)
{
    qtest_start(
        "-machine q35,cxl=on "
        "-device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 "
        "-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.size=4G "
        "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 "
        "-device cxl-rp,id=rp1,bus=cxl.0,chassis=0,slot=1 "
        "-device pcie-accelerator-cxl,bus=rp0,id=accel0 "
        "-object memory-backend-ram,id=cxl-mem0,size=256M "
        "-device cxl-type3,bus=rp1,volatile-memdev=cxl-mem0,id=mem0 "
    );
    qtest_end();
}

/* ===== Test: Two pxb-cxl bridges with accelerators ===== */

static void test_cxl_accel_2pxb(void)
{
    qtest_start(
        "-machine q35,cxl=on "
        "-device pxb-cxl,id=cxl.0,bus=pcie.0,bus_nr=52 "
        "-device pxb-cxl,id=cxl.1,bus=pcie.0,bus_nr=53 "
        "-M cxl-fmw.0.targets.0=cxl.0,cxl-fmw.0.targets.1=cxl.1,"
        "cxl-fmw.0.size=4G "
        "-device cxl-rp,id=rp0,bus=cxl.0,chassis=0,slot=0 "
        "-device cxl-rp,id=rp1,bus=cxl.1,chassis=0,slot=1 "
        "-device pcie-accelerator-cxl,bus=rp0,id=accel0 "
        "-device pcie-accelerator-cxl,bus=rp1,id=accel1 "
    );
    qtest_end();
}

/* ===== main ===== */

int main(int argc, char **argv)
{
    const char *arch = qtest_get_arch();

    g_test_init(&argc, &argv, NULL);

    if (strcmp(arch, "i386") == 0 || strcmp(arch, "x86_64") == 0) {
        qtest_add_func("/pci/cxl/accel/instantiate",
                        test_cxl_accel_instantiate);
        qtest_add_func("/pci/cxl/accel/instantiate_dpa",
                        test_cxl_accel_instantiate_dpa);
        qtest_add_func("/pci/cxl/accel/cache_lines_prop",
                        test_cxl_accel_cache_lines_prop);
        qtest_add_func("/pci/cxl/accel/dual_devices",
                        test_cxl_accel_dual_devices);
        qtest_add_func("/pci/cxl/accel/dual_with_dpa",
                        test_cxl_accel_dual_with_dpa);
        qtest_add_func("/pci/cxl/accel/with_type3",
                        test_cxl_accel_with_type3);
        qtest_add_func("/pci/cxl/accel/2pxb",
                        test_cxl_accel_2pxb);
    }

    return g_test_run();
}
