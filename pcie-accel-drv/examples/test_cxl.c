/* SPDX-License-Identifier: MIT */
/*
 * PCIe Accelerator Device - CXL Memory Test
 *
 * Copyright (C) 2026
 *
 * This test program verifies CXL Type 1 memory expander functionality.
 * Tests read/write operations to CXL-attached memory via the accelerator
 * device's DMA engine.
 *
 * Usage: test_cxl [options]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <getopt.h>

#include "../libaccel/libaccel.h"

#define DEFAULT_DEVICE      "/dev/accel0"
#define DEFAULT_SIZE        4096
#define DEFAULT_ITERATIONS  100
#define DEFAULT_OFFSET      0

/*
 * ===== Test Patterns =====
 */

/**
 * fill_test_pattern - Fill buffer with deterministic test pattern
 */
static void fill_test_pattern(void *buf, size_t len, uint32_t seed)
{
    uint32_t *data = buf;
    size_t count = len / sizeof(uint32_t);

    for (size_t i = 0; i < count; i++)
        data[i] = seed + i;

    /* Handle remaining bytes */
    uint8_t *tail = (uint8_t *)(data + count);
    size_t remaining = len % sizeof(uint32_t);
    for (size_t i = 0; i < remaining; i++)
        tail[i] = (seed + count + i) & 0xFF;
}

/**
 * verify_pattern - Verify buffer contains expected pattern
 */
static int verify_pattern(const void *buf, size_t len, uint32_t seed)
{
    const uint32_t *data = buf;
    size_t count = len / sizeof(uint32_t);
    int errors = 0;

    for (size_t i = 0; i < count; i++) {
        uint32_t expected = seed + i;
        if (data[i] != expected) {
            if (errors < 10) {
                fprintf(stderr, "  Mismatch at offset %zu: "
                        "expected 0x%08x, got 0x%08x\n",
                        i * sizeof(uint32_t), expected, data[i]);
            }
            errors++;
        }
    }

    return errors;
}

/**
 * fill_random_data - Fill buffer with pseudo-random data
 */
static void fill_random_data(void *buf, size_t len)
{
    uint32_t *data = buf;
    size_t count = len / sizeof(uint32_t);

    for (size_t i = 0; i < count; i++)
        data[i] = rand();

    uint8_t *tail = (uint8_t *)(data + count);
    size_t remaining = len % sizeof(uint32_t);
    for (size_t i = 0; i < remaining; i++)
        tail[i] = rand() & 0xFF;
}

/**
 * compare_buffers - Compare two buffers
 */
static int compare_buffers(const void *buf1, const void *buf2, size_t len)
{
    const uint8_t *b1 = buf1;
    const uint8_t *b2 = buf2;
    int errors = 0;

    for (size_t i = 0; i < len; i++) {
        if (b1[i] != b2[i]) {
            if (errors < 10) {
                fprintf(stderr, "  Mismatch at offset %zu: "
                        "expected 0x%02x, got 0x%02x\n",
                        i, b1[i], b2[i]);
            }
            errors++;
        }
    }

    return errors;
}

/*
 * ===== Basic CXL Tests =====
 */

/**
 * test_cxl_write_read - Basic write then read test
 */
static int test_cxl_write_read(struct accel_device *dev, uint16_t qid,
                               uint64_t cxl_addr, size_t size, uint32_t seed)
{
    void *write_buf = NULL;
    void *read_buf = NULL;
    int ret = -1;

    /* Allocate aligned buffers */
    if (posix_memalign(&write_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate write buffer\n");
        return -1;
    }

    if (posix_memalign(&read_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate read buffer\n");
        goto out;
    }

    /* Fill write buffer with pattern */
    fill_test_pattern(write_buf, size, seed);
    memset(read_buf, 0, size);

    /* Write to CXL memory */
    ret = accel_cxl_write(dev, qid, write_buf, cxl_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "CXL write failed: %s\n", accel_strerror(ret));
        ret = -1;
        goto out;
    }

    /* Read back from CXL memory */
    ret = accel_cxl_read(dev, qid, read_buf, cxl_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "CXL read failed: %s\n", accel_strerror(ret));
        ret = -1;
        goto out;
    }

    /* Verify data */
    int errors = compare_buffers(write_buf, read_buf, size);
    if (errors > 0) {
        fprintf(stderr, "Data verification failed: %d errors\n", errors);
        ret = -1;
        goto out;
    }

    ret = 0;

out:
    free(write_buf);
    free(read_buf);
    return ret;
}

/**
 * test_cxl_random_data - Test with random data patterns
 */
static int test_cxl_random_data(struct accel_device *dev, uint16_t qid,
                                uint64_t cxl_addr, size_t size)
{
    void *write_buf = NULL;
    void *read_buf = NULL;
    int ret = -1;

    if (posix_memalign(&write_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate write buffer\n");
        return -1;
    }

    if (posix_memalign(&read_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate read buffer\n");
        goto out;
    }

    /* Fill with random data */
    fill_random_data(write_buf, size);
    memset(read_buf, 0xAA, size);

    /* Write to CXL memory */
    ret = accel_cxl_write(dev, qid, write_buf, cxl_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "CXL write failed: %s\n", accel_strerror(ret));
        ret = -1;
        goto out;
    }

    /* Read back */
    ret = accel_cxl_read(dev, qid, read_buf, cxl_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "CXL read failed: %s\n", accel_strerror(ret));
        ret = -1;
        goto out;
    }

    /* Verify */
    int errors = compare_buffers(write_buf, read_buf, size);
    if (errors > 0) {
        fprintf(stderr, "Random data verification failed: %d errors\n", errors);
        ret = -1;
        goto out;
    }

    ret = 0;

out:
    free(write_buf);
    free(read_buf);
    return ret;
}

/**
 * test_cxl_address_walk - Test different addresses in CXL memory
 */
static int test_cxl_address_walk(struct accel_device *dev, uint16_t qid,
                                 uint64_t cxl_size, size_t xfer_size)
{
    void *buf = NULL;
    int ret = -1;
    int passed = 0;
    int failed = 0;

    if (posix_memalign(&buf, 4096, xfer_size) != 0) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return -1;
    }

    printf("  Walking CXL address space (size=%zu, xfer=%zu)...\n",
           (size_t)cxl_size, xfer_size);

    /* Test at various offsets */
    uint64_t offsets[] = {
        0,                              /* Start */
        xfer_size,                      /* After first block */
        cxl_size / 4,                   /* Quarter */
        cxl_size / 2,                   /* Middle */
        cxl_size - xfer_size,           /* End */
    };

    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        uint64_t addr = offsets[i];

        /* Skip if address is out of bounds */
        if (addr + xfer_size > cxl_size)
            continue;

        printf("    Testing offset 0x%lx... ", (unsigned long)addr);
        fflush(stdout);

        /* Write pattern */
        fill_test_pattern(buf, xfer_size, (uint32_t)addr);
        ret = accel_cxl_write(dev, qid, buf, addr, xfer_size);
        if (ret != ACCEL_SUCCESS) {
            printf("WRITE FAILED: %s\n", accel_strerror(ret));
            failed++;
            continue;
        }

        /* Clear and read back */
        memset(buf, 0, xfer_size);
        ret = accel_cxl_read(dev, qid, buf, addr, xfer_size);
        if (ret != ACCEL_SUCCESS) {
            printf("READ FAILED: %s\n", accel_strerror(ret));
            failed++;
            continue;
        }

        /* Verify */
        int errors = verify_pattern(buf, xfer_size, (uint32_t)addr);
        if (errors > 0) {
            printf("VERIFY FAILED: %d errors\n", errors);
            failed++;
        } else {
            printf("OK\n");
            passed++;
        }
    }

    free(buf);
    printf("  Address walk: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? -1 : 0;
}

/**
 * test_cxl_size_variations - Test different transfer sizes
 */
static int test_cxl_size_variations(struct accel_device *dev, uint16_t qid,
                                    uint64_t cxl_addr)
{
    int passed = 0;
    int failed = 0;

    printf("  Testing various transfer sizes...\n");

    size_t sizes[] = {
        64,         /* Minimum */
        256,
        512,
        1024,
        4096,       /* Page size */
        8192,
        16384,
        65536,      /* 64KB */
        262144,     /* 256KB */
        1048576,    /* 1MB */
    };

    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t size = sizes[i];

        printf("    %zu bytes... ", size);
        fflush(stdout);

        int ret = test_cxl_write_read(dev, qid, cxl_addr, size, 0xDEADBEEF);
        if (ret == 0) {
            printf("OK\n");
            passed++;
        } else {
            printf("FAILED\n");
            failed++;
        }
    }

    printf("  Size variations: %d passed, %d failed\n", passed, failed);
    return failed > 0 ? -1 : 0;
}

/**
 * test_cxl_stress - Stress test with many iterations
 */
static int test_cxl_stress(struct accel_device *dev, uint16_t qid,
                           uint64_t cxl_addr, size_t size, int iterations)
{
    void *write_buf = NULL;
    void *read_buf = NULL;
    int passed = 0;
    int failed = 0;
    int ret = -1;

    if (posix_memalign(&write_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate write buffer\n");
        return -1;
    }

    if (posix_memalign(&read_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate read buffer\n");
        free(write_buf);
        return -1;
    }

    printf("  Running %d stress iterations (size=%zu)...\n", iterations, size);

    for (int i = 0; i < iterations; i++) {
        uint32_t seed = (uint32_t)i * 0x12345678;

        /* Fill with pattern */
        fill_test_pattern(write_buf, size, seed);

        /* Write */
        ret = accel_cxl_write(dev, qid, write_buf, cxl_addr, size);
        if (ret != ACCEL_SUCCESS) {
            failed++;
            continue;
        }

        /* Read back */
        memset(read_buf, 0, size);
        ret = accel_cxl_read(dev, qid, read_buf, cxl_addr, size);
        if (ret != ACCEL_SUCCESS) {
            failed++;
            continue;
        }

        /* Verify */
        int errors = compare_buffers(write_buf, read_buf, size);
        if (errors > 0) {
            failed++;
        } else {
            passed++;
        }

        /* Progress */
        if ((i + 1) % 10 == 0) {
            printf("\r    Progress: %d/%d", i + 1, iterations);
            fflush(stdout);
        }
    }

    printf("\n  Stress test: %d passed, %d failed\n", passed, failed);

    free(write_buf);
    free(read_buf);
    return failed > 0 ? -1 : 0;
}

/**
 * test_cxl_persistence - Test that data persists in CXL memory
 */
static int test_cxl_persistence(struct accel_device *dev, uint16_t qid,
                                uint64_t cxl_addr, size_t size)
{
    void *buf1 = NULL;
    void *buf2 = NULL;
    void *buf3 = NULL;
    int ret = -1;

    printf("  Testing data persistence...\n");

    if (posix_memalign(&buf1, 4096, size) != 0 ||
        posix_memalign(&buf2, 4096, size) != 0 ||
        posix_memalign(&buf3, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate buffers\n");
        goto out;
    }

    /* Write pattern 1 */
    fill_test_pattern(buf1, size, 0x11111111);
    ret = accel_cxl_write(dev, qid, buf1, cxl_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Write 1 failed: %s\n", accel_strerror(ret));
        goto out;
    }

    /* Write pattern 2 to a different location */
    fill_test_pattern(buf2, size, 0x22222222);
    ret = accel_cxl_write(dev, qid, buf2, cxl_addr + size, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Write 2 failed: %s\n", accel_strerror(ret));
        goto out;
    }

    /* Read back pattern 1 - should still be there */
    memset(buf3, 0, size);
    ret = accel_cxl_read(dev, qid, buf3, cxl_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Read 1 failed: %s\n", accel_strerror(ret));
        goto out;
    }

    int errors = compare_buffers(buf1, buf3, size);
    if (errors > 0) {
        fprintf(stderr, "Pattern 1 was corrupted: %d errors\n", errors);
        ret = -1;
        goto out;
    }

    /* Read back pattern 2 */
    memset(buf3, 0, size);
    ret = accel_cxl_read(dev, qid, buf3, cxl_addr + size, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Read 2 failed: %s\n", accel_strerror(ret));
        goto out;
    }

    errors = compare_buffers(buf2, buf3, size);
    if (errors > 0) {
        fprintf(stderr, "Pattern 2 was corrupted: %d errors\n", errors);
        ret = -1;
        goto out;
    }

    printf("    Data persistence verified\n");
    ret = 0;

out:
    free(buf1);
    free(buf2);
    free(buf3);
    return ret;
}

/*
 * ===== Main =====
 */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -d <device>     Device path (default: %s)\n", DEFAULT_DEVICE);
    printf("  -s <size>       Transfer size in bytes (default: %d)\n", DEFAULT_SIZE);
    printf("  -o <offset>     CXL memory offset (default: %d)\n", DEFAULT_OFFSET);
    printf("  -n <count>      Stress test iterations (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  -q <qid>        Queue ID to use (default: 1)\n");
    printf("  -a              Run all tests\n");
    printf("  -h              Show this help\n");
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    size_t size = DEFAULT_SIZE;
    uint64_t offset = DEFAULT_OFFSET;
    int iterations = DEFAULT_ITERATIONS;
    uint16_t qid = 1;
    bool run_all = false;
    struct accel_device *dev;
    struct accel_identify id;
    int opt;
    int ret;
    int total_passed = 0;
    int total_failed = 0;

    /* Parse arguments */
    while ((opt = getopt(argc, argv, "d:s:o:n:q:ah")) != -1) {
        switch (opt) {
        case 'd':
            device = optarg;
            break;
        case 's':
            size = atoi(optarg);
            break;
        case 'o':
            offset = strtoull(optarg, NULL, 0);
            break;
        case 'n':
            iterations = atoi(optarg);
            break;
        case 'q':
            qid = atoi(optarg);
            break;
        case 'a':
            run_all = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    printf("PCIe Accelerator CXL Memory Test\n");
    printf("=================================\n");
    printf("Device:     %s\n", device);
    printf("Size:       %zu bytes\n", size);
    printf("Offset:     0x%lx\n", (unsigned long)offset);
    printf("Iterations: %d\n", iterations);
    printf("Queue ID:   %u\n", qid);
    printf("\n");

    /* Open device */
    dev = accel_open(device);
    if (!dev) {
        fprintf(stderr, "Failed to open device %s: %s\n",
                device, strerror(errno));
        return 1;
    }

    /* Get device identification */
    ret = accel_identify(dev, &id);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to identify device: %s\n",
                accel_strerror(ret));
        accel_close(dev);
        return 1;
    }

    printf("Device Information:\n");
    printf("  Version:      %u.%u.%u\n",
           (id.version >> 16) & 0xFFFF,
           (id.version >> 8) & 0xFF,
           id.version & 0xFF);
    printf("  Max queues:   %u\n", id.max_queues);
    printf("  Max q size:   %u\n", id.max_queue_size);
    printf("  CXL memory:   %lu MB (%lu bytes)\n",
           (unsigned long)(id.cxl_size / (1024 * 1024)),
           (unsigned long)id.cxl_size);
    printf("\n");

    /* Check if CXL memory is available */
    if (id.cxl_size == 0) {
        fprintf(stderr, "ERROR: Device has no CXL memory!\n");
        fprintf(stderr, "Make sure the QEMU device was started with CXL support:\n");
        fprintf(stderr, "  -device pcie-accel,cxl_mem_size=256M\n");
        accel_close(dev);
        return 1;
    }

    /* Validate offset and size */
    if (offset + size > id.cxl_size) {
        fprintf(stderr, "ERROR: Offset 0x%lx + size %zu exceeds CXL memory size %lu\n",
                (unsigned long)offset, size, (unsigned long)id.cxl_size);
        accel_close(dev);
        return 1;
    }

    /* Create I/O queue */
    ret = accel_create_queue(dev, qid, 256, 256);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to create queue %u: %s\n",
                qid, accel_strerror(ret));
        accel_close(dev);
        return 1;
    }

    /* Seed random number generator */
    srand(time(NULL));

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /*
     * Run tests
     */
    printf("Running CXL memory tests...\n\n");

    /* Test 1: Basic write/read */
    printf("[Test 1] Basic write/read...\n");
    ret = test_cxl_write_read(dev, qid, offset, size, 0xCAFEBABE);
    if (ret == 0) {
        printf("  PASSED\n\n");
        total_passed++;
    } else {
        printf("  FAILED\n\n");
        total_failed++;
    }

    /* Test 2: Random data */
    printf("[Test 2] Random data...\n");
    ret = test_cxl_random_data(dev, qid, offset, size);
    if (ret == 0) {
        printf("  PASSED\n\n");
        total_passed++;
    } else {
        printf("  FAILED\n\n");
        total_failed++;
    }

    /* Test 3: Data persistence */
    printf("[Test 3] Data persistence...\n");
    if (offset + size * 2 <= id.cxl_size) {
        ret = test_cxl_persistence(dev, qid, offset, size);
        if (ret == 0) {
            printf("  PASSED\n\n");
            total_passed++;
        } else {
            printf("  FAILED\n\n");
            total_failed++;
        }
    } else {
        printf("  SKIPPED (not enough CXL memory)\n\n");
    }

    if (run_all) {
        /* Test 4: Size variations */
        printf("[Test 4] Size variations...\n");
        ret = test_cxl_size_variations(dev, qid, offset);
        if (ret == 0) {
            printf("  PASSED\n\n");
            total_passed++;
        } else {
            printf("  FAILED\n\n");
            total_failed++;
        }

        /* Test 5: Address walk */
        printf("[Test 5] Address walk...\n");
        ret = test_cxl_address_walk(dev, qid, id.cxl_size, size);
        if (ret == 0) {
            printf("  PASSED\n\n");
            total_passed++;
        } else {
            printf("  FAILED\n\n");
            total_failed++;
        }

        /* Test 6: Stress test */
        printf("[Test 6] Stress test...\n");
        ret = test_cxl_stress(dev, qid, offset, size, iterations);
        if (ret == 0) {
            printf("  PASSED\n\n");
            total_passed++;
        } else {
            printf("  FAILED\n\n");
            total_failed++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    /* Summary */
    printf("========================================\n");
    printf("CXL Memory Test Results\n");
    printf("========================================\n");
    printf("  Total tests: %d\n", total_passed + total_failed);
    printf("  Passed:      %d\n", total_passed);
    printf("  Failed:      %d\n", total_failed);
    printf("  Time:        %.3f seconds\n", elapsed);
    printf("\n");

    if (total_failed == 0) {
        printf("ALL TESTS PASSED!\n");
    } else {
        printf("SOME TESTS FAILED!\n");
    }

    /* Cleanup */
    accel_delete_queue(dev, qid);
    accel_close(dev);

    return total_failed > 0 ? 1 : 0;
}
