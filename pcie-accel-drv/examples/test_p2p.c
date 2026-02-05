/* SPDX-License-Identifier: MIT */
/*
 * PCIe Accelerator Device - P2P DMA Test with io_uring
 *
 * Copyright (C) 2026
 *
 * This test program verifies peer-to-peer DMA functionality between
 * two PCIe Accelerator devices. Supports both synchronous and asynchronous
 * modes with bidirectional concurrent transfers using io_uring.
 *
 * Usage: test_p2p [options]
 *
 * Note: Requires two PCIe Accelerator devices in the same QEMU instance.
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

#define DEFAULT_DEVICE1     "/dev/accel0"
#define DEFAULT_DEVICE2     "/dev/accel1"
#define DEFAULT_SIZE        65536
#define DEFAULT_ITERATIONS  10
#define DEFAULT_CONCURRENT  4

/**
 * P2P transfer context for async operations
 */
struct p2p_context {
    struct accel_device *dev;
    uint16_t peer_bdf;
    uint16_t qid;
    void *buffer;
    size_t size;
    uint64_t offset;
    uint32_t seed;
    bool is_write;
    struct accel_async_token token;
    bool completed;
    int result;
};

/**
 * get_device_bdf - Get BDF from device (simplified)
 *
 * In a real implementation, this would read from sysfs.
 * For testing, we use a simple mapping.
 */
static uint16_t get_device_bdf(const char *path)
{
    /*
     * Parse device number from path and generate a BDF.
     * This is simplified - real implementation would read from sysfs.
     *
     * For QEMU testing, devices are typically at fixed BDFs like:
     * - accel0: 00:04.0 -> BDF = 0x0020
     * - accel1: 00:05.0 -> BDF = 0x0028
     */
    int dev_num = 0;
    const char *p = strrchr(path, 'l');  /* Find "accel" */
    if (p && p[1] >= '0' && p[1] <= '9')
        dev_num = p[1] - '0';

    /* Return BDF: bus=0, device=4+num, function=0 */
    return ((4 + dev_num) << 3);
}

/**
 * fill_test_pattern - Fill buffer with verifiable pattern
 */
static void fill_test_pattern(void *buf, size_t len, uint32_t seed)
{
    uint32_t *data = buf;
    size_t count = len / sizeof(uint32_t);

    for (size_t i = 0; i < count; i++)
        data[i] = seed + i;
}

/**
 * verify_test_pattern - Verify buffer contains expected pattern
 */
static int verify_test_pattern(const void *buf, size_t len, uint32_t seed)
{
    const uint32_t *data = buf;
    size_t count = len / sizeof(uint32_t);
    int errors = 0;

    for (size_t i = 0; i < count; i++) {
        uint32_t expected = seed + i;
        if (data[i] != expected) {
            if (errors < 10) {
                fprintf(stderr, "Pattern mismatch at offset %zu: "
                        "expected 0x%08x, got 0x%08x\n",
                        i * sizeof(uint32_t), expected, data[i]);
            }
            errors++;
        }
    }

    return errors;
}

/**
 * run_p2p_test_sync - Run P2P transfer test (synchronous)
 */
static int run_p2p_test_sync(struct accel_device *dev1,
                             struct accel_device *dev2 __attribute__((unused)),
                             uint16_t bdf2, uint16_t qid,
                             size_t size, int iteration)
{
    void *src_buf = NULL;
    void *dst_buf = NULL;
    uint32_t seed = iteration * 1000;
    int ret = -1;

    /* Allocate aligned buffers */
    if (posix_memalign(&src_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate source buffer\n");
        return -1;
    }

    if (posix_memalign(&dst_buf, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate destination buffer\n");
        goto out;
    }

    /* Fill source with test pattern */
    fill_test_pattern(src_buf, size, seed);
    memset(dst_buf, 0, size);

    /*
     * Test 1: P2P Write (dev1 -> dev2)
     */
    printf("  [%d] P2P Write: dev1 -> dev2 (%zu bytes)... ", iteration, size);
    fflush(stdout);

    ret = accel_p2p_write(dev1, qid, bdf2, src_buf, 0, size);
    if (ret != ACCEL_SUCCESS) {
        printf("FAILED: %s\n", accel_strerror(ret));
        goto out;
    }
    printf("OK\n");

    /*
     * Test 2: P2P Read (dev2 -> dev1)
     */
    printf("  [%d] P2P Read: dev2 -> dev1 (%zu bytes)... ", iteration, size);
    fflush(stdout);

    ret = accel_p2p_read(dev1, qid, bdf2, dst_buf, 0, size);
    if (ret != ACCEL_SUCCESS) {
        printf("FAILED: %s\n", accel_strerror(ret));
        goto out;
    }
    printf("OK\n");

    /*
     * Verify data integrity
     */
    printf("  [%d] Verifying data... ", iteration);
    fflush(stdout);

    int errors = verify_test_pattern(dst_buf, size, seed);
    if (errors > 0) {
        printf("FAILED: %d errors\n", errors);
        ret = -1;
        goto out;
    }
    printf("OK\n");

    ret = 0;

out:
    free(src_buf);
    free(dst_buf);
    return ret;
}

/**
 * alloc_p2p_contexts - Allocate P2P contexts for async operations
 */
static struct p2p_context *alloc_p2p_contexts(int count, size_t size)
{
    struct p2p_context *contexts;

    contexts = calloc(count, sizeof(*contexts));
    if (!contexts)
        return NULL;

    for (int i = 0; i < count; i++) {
        contexts[i].size = size;
        contexts[i].completed = false;
        contexts[i].result = -1;

        if (posix_memalign(&contexts[i].buffer, 4096, size) != 0)
            goto cleanup;
    }

    return contexts;

cleanup:
    for (int i = 0; i < count; i++)
        free(contexts[i].buffer);
    free(contexts);
    return NULL;
}

/**
 * free_p2p_contexts - Free P2P contexts
 */
static void free_p2p_contexts(struct p2p_context *contexts, int count)
{
    if (!contexts)
        return;

    for (int i = 0; i < count; i++)
        free(contexts[i].buffer);
    free(contexts);
}

/**
 * run_p2p_concurrent_async - Run concurrent P2P transfers using io_uring
 *
 * This demonstrates N:N concurrent transfers between two devices using
 * io_uring for maximum throughput.
 */
static int run_p2p_concurrent_async(struct accel_device *dev1,
                                    struct accel_device *dev2,
                                    uint16_t bdf1, uint16_t bdf2,
                                    uint16_t qid, size_t size,
                                    int total_iterations, int concurrent,
                                    int *passed, int *failed)
{
    struct p2p_context *write_contexts = NULL;
    struct p2p_context *read_contexts = NULL;
    struct accel_async_result *results = NULL;
    int ret = -1;
    int pair_submitted = 0;
    int pairs_completed = 0;

    /* Allocate contexts for concurrent operations */
    write_contexts = alloc_p2p_contexts(concurrent, size);
    read_contexts = alloc_p2p_contexts(concurrent, size);
    results = calloc(concurrent * 2, sizeof(*results));

    if (!write_contexts || !read_contexts || !results) {
        fprintf(stderr, "Failed to allocate contexts\n");
        goto out;
    }

    *passed = 0;
    *failed = 0;

    printf("Using io_uring concurrent P2P transfers (%d in flight)\n\n", concurrent);

    /* Begin batch mode on both devices */
    ret = accel_begin_batch(dev1);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to begin batch on dev1: %s\n",
                accel_strerror(ret));
        goto out;
    }

    ret = accel_begin_batch(dev2);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to begin batch on dev2: %s\n",
                accel_strerror(ret));
        goto out;
    }

    /* Submit initial batch of concurrent operations */
    for (int i = 0; i < concurrent && pair_submitted < total_iterations; i++) {
        uint32_t seed = pair_submitted * 1000;

        /* Setup write context (dev1 -> dev2) */
        write_contexts[i].dev = dev1;
        write_contexts[i].peer_bdf = bdf2;
        write_contexts[i].qid = qid;
        write_contexts[i].offset = i * size;  /* Use different offsets */
        write_contexts[i].seed = seed;
        write_contexts[i].is_write = true;
        write_contexts[i].completed = false;

        fill_test_pattern(write_contexts[i].buffer, size, seed);

        /* Queue async P2P write */
        ret = accel_async_p2p_write(dev1, qid, bdf2, write_contexts[i].buffer,
                                    write_contexts[i].offset, size,
                                    &write_contexts[i].token);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to queue P2P write %d: %s\n",
                    i, accel_strerror(ret));
            goto out;
        }

        /* Setup read context (dev2 -> dev1) for verification later */
        read_contexts[i].dev = dev1;
        read_contexts[i].peer_bdf = bdf2;
        read_contexts[i].qid = qid;
        read_contexts[i].offset = i * size;
        read_contexts[i].seed = seed;
        read_contexts[i].is_write = false;
        read_contexts[i].completed = false;

        memset(read_contexts[i].buffer, 0, size);

        pair_submitted++;
    }

    /* Submit writes on dev1 */
    ret = accel_submit_batch(dev1);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to submit batch on dev1: %s\n",
                accel_strerror(ret));
        goto out;
    }

    printf("Submitted %d concurrent P2P write operations\n", pair_submitted);

    /* Wait for all writes to complete before issuing reads */
    int writes_completed = 0;
    while (writes_completed < pair_submitted) {
        struct accel_async_result result;

        ret = accel_wait_completion(dev1, &result, 1000);
        if (ret == ACCEL_ERR_TIMEOUT) {
            fprintf(stderr, "Timeout waiting for P2P writes\n");
            goto out;
        }
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Error waiting for completion: %s\n",
                    accel_strerror(ret));
            goto out;
        }

        /* Find matching context */
        for (int i = 0; i < pair_submitted; i++) {
            if (write_contexts[i].token.user_data == result.token.user_data) {
                write_contexts[i].completed = true;
                write_contexts[i].result = result.result;
                writes_completed++;
                break;
            }
        }
    }

    printf("All %d P2P writes completed\n", writes_completed);

    /* Now issue reads to verify data */
    accel_begin_batch(dev1);

    for (int i = 0; i < pair_submitted; i++) {
        ret = accel_async_p2p_read(dev1, qid, bdf2, read_contexts[i].buffer,
                                   read_contexts[i].offset, size,
                                   &read_contexts[i].token);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to queue P2P read %d: %s\n",
                    i, accel_strerror(ret));
            goto out;
        }
    }

    ret = accel_submit_batch(dev1);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to submit reads: %s\n",
                accel_strerror(ret));
        goto out;
    }

    printf("Submitted %d concurrent P2P read operations\n", pair_submitted);

    /* Wait for reads and verify */
    int reads_completed = 0;
    while (reads_completed < pair_submitted) {
        struct accel_async_result result;

        ret = accel_wait_completion(dev1, &result, 1000);
        if (ret == ACCEL_ERR_TIMEOUT) {
            fprintf(stderr, "Timeout waiting for P2P reads\n");
            goto out;
        }
        if (ret != ACCEL_SUCCESS && ret != ACCEL_ERR_TIMEOUT) {
            fprintf(stderr, "Error waiting for read: %s\n",
                    accel_strerror(ret));
            goto out;
        }

        /* Find and verify */
        for (int i = 0; i < pair_submitted; i++) {
            if (read_contexts[i].token.user_data == result.token.user_data) {
                read_contexts[i].completed = true;
                reads_completed++;

                if (result.result == ACCEL_SUCCESS) {
                    int errors = verify_test_pattern(read_contexts[i].buffer,
                                                     size,
                                                     read_contexts[i].seed);
                    if (errors == 0) {
                        (*passed)++;
                    } else {
                        fprintf(stderr, "Verification failed for pair %d\n", i);
                        (*failed)++;
                    }
                } else {
                    fprintf(stderr, "P2P read %d failed: %d\n",
                            i, result.result);
                    (*failed)++;
                }
                break;
            }
        }

        printf("\rProgress: %d/%d verified", reads_completed, pair_submitted);
        fflush(stdout);
    }

    printf("\n");
    pairs_completed = pair_submitted;
    ret = 0;

out:
    free(results);
    free_p2p_contexts(write_contexts, concurrent);
    free_p2p_contexts(read_contexts, concurrent);
    return ret;
}

/**
 * run_bidirectional_async - Run bidirectional P2P transfers simultaneously
 *
 * This demonstrates full-duplex P2P DMA using io_uring, with dev1->dev2 and
 * dev2->dev1 transfers happening concurrently.
 */
static int run_bidirectional_async(struct accel_device *dev1,
                                   struct accel_device *dev2,
                                   uint16_t bdf1, uint16_t bdf2,
                                   uint16_t qid, size_t size,
                                   int iterations,
                                   int *passed, int *failed)
{
    void *buf1_to_2 = NULL;
    void *buf2_to_1 = NULL;
    void *verify1 = NULL;
    void *verify2 = NULL;
    struct accel_async_token token1, token2;
    int ret = -1;

    *passed = 0;
    *failed = 0;

    /* Allocate buffers */
    if (posix_memalign(&buf1_to_2, 4096, size) != 0 ||
        posix_memalign(&buf2_to_1, 4096, size) != 0 ||
        posix_memalign(&verify1, 4096, size) != 0 ||
        posix_memalign(&verify2, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate buffers\n");
        goto out;
    }

    printf("Running bidirectional P2P test (%d iterations)\n\n", iterations);

    for (int i = 0; i < iterations; i++) {
        uint32_t seed1 = i * 2000;
        uint32_t seed2 = i * 2000 + 1000;

        /* Fill buffers with different patterns */
        fill_test_pattern(buf1_to_2, size, seed1);
        fill_test_pattern(buf2_to_1, size, seed2);
        memset(verify1, 0, size);
        memset(verify2, 0, size);

        printf("  [%d] Starting bidirectional transfer...\n", i);

        /* Begin batch mode on both devices */
        accel_begin_batch(dev1);
        accel_begin_batch(dev2);

        /* Queue dev1 -> dev2 write */
        ret = accel_async_p2p_write(dev1, qid, bdf2, buf1_to_2, 0, size, &token1);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to queue dev1->dev2 write: %s\n",
                    accel_strerror(ret));
            (*failed)++;
            continue;
        }

        /* Queue dev2 -> dev1 write */
        ret = accel_async_p2p_write(dev2, qid, bdf1, buf2_to_1, 0, size, &token2);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to queue dev2->dev1 write: %s\n",
                    accel_strerror(ret));
            (*failed)++;
            continue;
        }

        /* Submit both batches simultaneously for true parallelism */
        accel_submit_batch(dev1);
        accel_submit_batch(dev2);

        /* Wait for both completions */
        struct accel_async_result result1, result2;
        int got1 = 0, got2 = 0;

        while (!got1 || !got2) {
            struct accel_async_result result;

            if (!got1) {
                ret = accel_wait_completion(dev1, &result, 100);
                if (ret == ACCEL_SUCCESS) {
                    result1 = result;
                    got1 = 1;
                }
            }

            if (!got2) {
                ret = accel_wait_completion(dev2, &result, 100);
                if (ret == ACCEL_SUCCESS) {
                    result2 = result;
                    got2 = 1;
                }
            }
        }

        printf("  [%d] Bidirectional writes completed\n", i);

        /* Now read back and verify from both directions */
        accel_begin_batch(dev1);
        accel_begin_batch(dev2);

        /* Read back what dev1 wrote to dev2 */
        ret = accel_async_p2p_read(dev1, qid, bdf2, verify1, 0, size, &token1);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to queue readback from dev2\n");
            (*failed)++;
            continue;
        }

        /* Read back what dev2 wrote to dev1 */
        ret = accel_async_p2p_read(dev2, qid, bdf1, verify2, 0, size, &token2);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to queue readback from dev1\n");
            (*failed)++;
            continue;
        }

        accel_submit_batch(dev1);
        accel_submit_batch(dev2);

        /* Wait for reads */
        got1 = got2 = 0;
        while (!got1 || !got2) {
            struct accel_async_result result;

            if (!got1) {
                ret = accel_wait_completion(dev1, &result, 100);
                if (ret == ACCEL_SUCCESS)
                    got1 = 1;
            }

            if (!got2) {
                ret = accel_wait_completion(dev2, &result, 100);
                if (ret == ACCEL_SUCCESS)
                    got2 = 1;
            }
        }

        /* Verify both directions */
        int errors1 = verify_test_pattern(verify1, size, seed1);
        int errors2 = verify_test_pattern(verify2, size, seed2);

        if (errors1 == 0 && errors2 == 0) {
            printf("  [%d] Bidirectional verification OK\n", i);
            (*passed)++;
        } else {
            printf("  [%d] Verification FAILED: dir1=%d errors, dir2=%d errors\n",
                   i, errors1, errors2);
            (*failed)++;
        }
    }

    ret = 0;

out:
    free(buf1_to_2);
    free(buf2_to_1);
    free(verify1);
    free(verify2);
    return ret;
}

/**
 * print_usage - Print usage information
 */
static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -1 <device>     First device path (default: %s)\n", DEFAULT_DEVICE1);
    printf("  -2 <device>     Second device path (default: %s)\n", DEFAULT_DEVICE2);
    printf("  -s <size>       Transfer size in bytes (default: %d)\n", DEFAULT_SIZE);
    printf("  -n <count>      Number of iterations (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  -c <count>      Concurrent operations for async mode (default: %d)\n",
           DEFAULT_CONCURRENT);
    printf("  -a              Use async mode with io_uring\n");
    printf("  -b              Use bidirectional async mode\n");
    printf("  -h              Show this help\n");
    printf("\nModes:\n");
    printf("  (default)       Synchronous write/read per iteration\n");
    printf("  -a              Concurrent async transfers using io_uring\n");
    printf("  -b              Bidirectional transfers (dev1<->dev2)\n");
    printf("\nNote: Both devices must exist and be accessible.\n");
}

int main(int argc, char *argv[])
{
    const char *device1 = DEFAULT_DEVICE1;
    const char *device2 = DEFAULT_DEVICE2;
    size_t size = DEFAULT_SIZE;
    int iterations = DEFAULT_ITERATIONS;
    int concurrent = DEFAULT_CONCURRENT;
    uint16_t qid = 1;
    bool async_mode = false;
    bool bidirectional = false;
    struct accel_device *dev1 = NULL;
    struct accel_device *dev2 = NULL;
    uint16_t bdf1, bdf2;
    int opt;
    int ret;

    /* Parse arguments */
    while ((opt = getopt(argc, argv, "1:2:s:n:c:abh")) != -1) {
        switch (opt) {
        case '1':
            device1 = optarg;
            break;
        case '2':
            device2 = optarg;
            break;
        case 's':
            size = atoi(optarg);
            break;
        case 'n':
            iterations = atoi(optarg);
            break;
        case 'c':
            concurrent = atoi(optarg);
            break;
        case 'a':
            async_mode = true;
            break;
        case 'b':
            bidirectional = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    printf("PCIe Accelerator P2P DMA Test (io_uring)\n");
    printf("========================================\n");
    printf("Device 1:   %s\n", device1);
    printf("Device 2:   %s\n", device2);
    printf("Size:       %zu bytes\n", size);
    printf("Iterations: %d\n", iterations);
    printf("Mode:       %s\n",
           bidirectional ? "bidirectional async" :
           async_mode ? "concurrent async" : "synchronous");
    if (async_mode)
        printf("Concurrent: %d\n", concurrent);
    printf("\n");

    /* Get BDFs */
    bdf1 = get_device_bdf(device1);
    bdf2 = get_device_bdf(device2);
    printf("BDF 1: 0x%04x (%02x:%02x.%x)\n",
           bdf1, bdf1 >> 8, (bdf1 >> 3) & 0x1F, bdf1 & 0x7);
    printf("BDF 2: 0x%04x (%02x:%02x.%x)\n",
           bdf2, bdf2 >> 8, (bdf2 >> 3) & 0x1F, bdf2 & 0x7);
    printf("\n");

    /* Open devices */
    dev1 = accel_open(device1);
    if (!dev1) {
        fprintf(stderr, "Failed to open device %s: %s\n",
                device1, strerror(errno));
        ret = 1;
        goto cleanup;
    }

    dev2 = accel_open(device2);
    if (!dev2) {
        fprintf(stderr, "Failed to open device %s: %s\n",
                device2, strerror(errno));
        ret = 1;
        goto cleanup;
    }

    printf("Device Information:\n");
    printf("  Device 1 io_uring fd: %d\n", accel_get_uring_fd(dev1));
    printf("  Device 2 io_uring fd: %d\n", accel_get_uring_fd(dev2));
    printf("\n");

    /* Create I/O queues on both devices */
    ret = accel_create_queue(dev1, qid, 256, 256);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to create queue on dev1: %s\n",
                accel_strerror(ret));
        ret = 1;
        goto cleanup;
    }

    ret = accel_create_queue(dev2, qid, 256, 256);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to create queue on dev2: %s\n",
                accel_strerror(ret));
        ret = 1;
        goto cleanup;
    }

    /* Register P2P peers on both devices for bidirectional */
    printf("Registering P2P peers...\n");
    ret = accel_setup_p2p_peer(dev1, bdf2);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to setup P2P peer on dev1: %s\n",
                accel_strerror(ret));
        ret = 1;
        goto cleanup;
    }

    ret = accel_setup_p2p_peer(dev2, bdf1);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to setup P2P peer on dev2: %s\n",
                accel_strerror(ret));
        ret = 1;
        goto cleanup;
    }
    printf("P2P peers registered successfully\n\n");

    /* Run tests */
    printf("Running P2P tests...\n\n");

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int passed = 0;
    int failed = 0;

    if (bidirectional) {
        /* Bidirectional async test */
        ret = run_bidirectional_async(dev1, dev2, bdf1, bdf2, qid, size,
                                      iterations, &passed, &failed);
    } else if (async_mode) {
        /* Concurrent async test */
        ret = run_p2p_concurrent_async(dev1, dev2, bdf1, bdf2, qid, size,
                                       iterations, concurrent,
                                       &passed, &failed);
    } else {
        /* Synchronous test */
        for (int i = 0; i < iterations; i++) {
            if (run_p2p_test_sync(dev1, dev2, bdf2, qid, size, i) == 0) {
                passed++;
            } else {
                failed++;
            }
            printf("\n");
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    /* Calculate elapsed time */
    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    /* Get statistics */
    struct accel_stats stats1, stats2;
    accel_get_stats(dev1, &stats1);
    accel_get_stats(dev2, &stats2);

    /* Print results */
    printf("\nResults:\n");
    printf("========\n");
    printf("  Passed:       %d\n", passed);
    printf("  Failed:       %d\n", failed);
    printf("  Total time:   %.3f seconds\n", elapsed);

    /* Each iteration does 1 write + 1 read (2x for bidirectional) */
    double multiplier = bidirectional ? 4.0 : 2.0;
    double total_bytes = (double)passed * size * multiplier;
    printf("  Total data:   %.2f MB\n", total_bytes / (1024.0 * 1024.0));
    printf("  Throughput:   %.2f MB/s\n",
           (total_bytes / (1024.0 * 1024.0)) / elapsed);
    printf("\n");

    printf("Device 1 Statistics:\n");
    printf("  Commands: %lu submitted, %lu completed\n",
           stats1.cmd_submitted, stats1.cmd_completed);
    printf("  io_uring: %lu submitted, %lu completed\n",
           stats1.uring_submissions, stats1.uring_completions);
    printf("\n");

    printf("Device 2 Statistics:\n");
    printf("  Commands: %lu submitted, %lu completed\n",
           stats2.cmd_submitted, stats2.cmd_completed);
    printf("  io_uring: %lu submitted, %lu completed\n",
           stats2.uring_submissions, stats2.uring_completions);
    printf("\n");

    ret = failed > 0 ? 1 : 0;

cleanup:
    /* Cleanup queues */
    if (dev1)
        accel_delete_queue(dev1, qid);
    if (dev2)
        accel_delete_queue(dev2, qid);

    /* Close devices */
    if (dev1)
        accel_close(dev1);
    if (dev2)
        accel_close(dev2);

    return ret;
}
