/* SPDX-License-Identifier: MIT */
/*
 * PCIe Accelerator Device - Memory Read/Write Test
 *
 * Copyright (C) 2026
 *
 * Tests MEM_WRITE (host->device) and MEM_READ (device->host) commands
 * by writing data to device memory and reading it back to verify integrity.
 *
 * Usage: test_mem [options]
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
#define DEFAULT_DEV_ADDR    0

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
 * verify_data - Compare two buffers and report mismatches
 */
static int verify_data(const void *expected, const void *actual,
                       size_t len)
{
    const uint8_t *exp = expected;
    const uint8_t *act = actual;
    int errors = 0;

    for (size_t i = 0; i < len; i++) {
        if (exp[i] != act[i]) {
            if (errors < 10) {
                fprintf(stderr, "Mismatch at offset %zu: "
                        "expected 0x%02x, got 0x%02x\n",
                        i, exp[i], act[i]);
            }
            errors++;
        }
    }

    return errors;
}

/**
 * run_mem_test_sync - Write data to device, read back, and verify
 */
static int run_mem_test_sync(struct accel_device *dev, uint16_t qid,
                             size_t size, uint64_t dev_addr)
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

    /* Fill write buffer with random data */
    fill_random_data(write_buf, size);
    memset(read_buf, 0, size);

    /* Write to device memory */
    ret = accel_mem_write(dev, qid, write_buf, dev_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "MEM_WRITE failed: %s\n", accel_strerror(ret));
        ret = -1;
        goto out;
    }

    /* Read back from device memory */
    ret = accel_mem_read(dev, qid, read_buf, dev_addr, size);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "MEM_READ failed: %s\n", accel_strerror(ret));
        ret = -1;
        goto out;
    }

    /* Verify data integrity */
    int errors = verify_data(write_buf, read_buf, size);
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
 * print_usage - Print usage information
 */
static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -d <device>     Device path (default: %s)\n", DEFAULT_DEVICE);
    printf("  -s <size>       Transfer size in bytes (default: %d)\n", DEFAULT_SIZE);
    printf("  -n <count>      Number of iterations (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  -a <addr>       Device memory offset (default: 0x%x)\n", DEFAULT_DEV_ADDR);
    printf("  -q <qid>        Queue ID to use (default: 1)\n");
    printf("  -h              Show this help\n");
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    size_t size = DEFAULT_SIZE;
    int iterations = DEFAULT_ITERATIONS;
    uint64_t dev_addr = DEFAULT_DEV_ADDR;
    uint16_t qid = 1;
    struct accel_device *dev;
    struct accel_identify id;
    int opt;
    int ret;

    while ((opt = getopt(argc, argv, "d:s:n:a:q:h")) != -1) {
        switch (opt) {
        case 'd':
            device = optarg;
            break;
        case 's':
            size = atoi(optarg);
            break;
        case 'n':
            iterations = atoi(optarg);
            break;
        case 'a':
            dev_addr = strtoull(optarg, NULL, 0);
            break;
        case 'q':
            qid = atoi(optarg);
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    printf("PCIe Accelerator Memory Read/Write Test\n");
    printf("========================================\n");
    printf("Device:     %s\n", device);
    printf("Size:       %zu bytes\n", size);
    printf("Dev addr:   0x%" PRIx64 "\n", dev_addr);
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
    printf("\n");

    /* Create I/O queue */
    ret = accel_create_queue(dev, qid);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to create queue %u: %s\n",
                qid, accel_strerror(ret));
        accel_close(dev);
        return 1;
    }

    srand(time(NULL));

    printf("Running %d write/read/verify iterations...\n", iterations);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int passed = 0;
    int failed = 0;

    for (int i = 0; i < iterations; i++) {
        if (run_mem_test_sync(dev, qid, size, dev_addr) == 0) {
            passed++;
        } else {
            failed++;
        }

        if ((i + 1) % 10 == 0) {
            printf("\rProgress: %d/%d", i + 1, iterations);
            fflush(stdout);
        }
    }
    printf("\n");

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("\n");

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("Results:\n");
    printf("  Passed:       %d\n", passed);
    printf("  Failed:       %d\n", failed);
    printf("  Total time:   %.3f seconds\n", elapsed);
    printf("  Throughput:   %.2f MB/s (write+read)\n",
           (passed * size * 2 / (1024.0 * 1024.0)) / elapsed);
    printf("  IOPS:         %.0f (write+read pairs)\n", passed / elapsed);
    printf("\n");

    accel_delete_queue(dev, qid);
    accel_close(dev);

    return failed > 0 ? 1 : 0;
}
