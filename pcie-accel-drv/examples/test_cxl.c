/* SPDX-License-Identifier: MIT */
/*
 * PCIe Accelerator Device - CXL Type 1 Test with io_uring
 *
 * Copyright (C) 2026
 *
 * Tests CXL.cache queue mode for both SQ (submission queue) reads and
 * CQ (completion queue) writes via the CXLQCFG register.  Supports
 * synchronous, async-batch, and concurrent async modes using io_uring.
 *
 * When CXL.cache is enabled (kernel sets CXLQCFG.EN=1 during probe):
 *   SQ: Device fetches SQEs via D2H RdOwn (64B cache-line granularity)
 *   CQ: Device posts CQEs via D2H WrCurr (read-modify-write for 16B CQEs)
 *
 * Usage: test_cxl [options]
 *   -d <device>   Device path (default: /dev/accel0)
 *   -s <size>     Transfer size in bytes (default: 4096)
 *   -n <count>    Iterations per test (default: 50)
 *   -q <qid>      Queue ID (default: 1)
 *   -a            Async batch mode (io_uring batch submission)
 *   -b <size>     Batch size for async mode (default: 32)
 *   -c            Concurrent async mode (keep N ops in flight)
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
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>

#include "../libaccel/libaccel.h"

#define DEFAULT_DEVICE      "/dev/accel0"
#define DEFAULT_SIZE        4096
#define DEFAULT_ITERATIONS  50
#define DEFAULT_BATCH_SIZE  32

/* ===== Test Context for Async Operations ===== */

struct test_cxl_ctx {
    void *original;
    void *buffer;
    size_t size;
    uint32_t pattern;
    int iteration;
    struct accel_async_token token;
    bool completed;
    int result;
};

static struct test_cxl_ctx *alloc_cxl_contexts(int count, size_t size,
                                                uint32_t pattern)
{
    struct test_cxl_ctx *ctxs = calloc(count, sizeof(*ctxs));
    int i;

    if (!ctxs)
        return NULL;

    for (i = 0; i < count; i++) {
        ctxs[i].size = size;
        ctxs[i].pattern = pattern;
        ctxs[i].iteration = i;
        ctxs[i].completed = false;
        ctxs[i].result = -1;

        if (posix_memalign(&ctxs[i].original, 4096, size) != 0)
            goto cleanup;
        if (posix_memalign(&ctxs[i].buffer, 4096, size) != 0)
            goto cleanup;
    }
    return ctxs;

cleanup:
    for (i = 0; i < count; i++) {
        free(ctxs[i].original);
        free(ctxs[i].buffer);
    }
    free(ctxs);
    return NULL;
}

static void free_cxl_contexts(struct test_cxl_ctx *ctxs, int count)
{
    int i;

    if (!ctxs)
        return;
    for (i = 0; i < count; i++) {
        free(ctxs[i].original);
        free(ctxs[i].buffer);
    }
    free(ctxs);
}

/* ===== Helpers ===== */

static void fill_random_data(void *buf, size_t len)
{
    uint32_t *data = buf;
    size_t count = len / sizeof(uint32_t);
    size_t i;

    for (i = 0; i < count; i++)
        data[i] = rand();
}

static int verify_loopback_data(const void *original, const void *result,
                                size_t len, uint32_t pattern)
{
    const uint32_t *orig = original;
    const uint32_t *res = result;
    size_t count = len / sizeof(uint32_t);
    int errors = 0;
    size_t i;

    for (i = 0; i < count; i++) {
        uint32_t expected = orig[i] ^ pattern;
        if (res[i] != expected) {
            if (errors < 5) {
                fprintf(stderr, "  Mismatch at offset %zu: "
                        "expected 0x%08x, got 0x%08x\n",
                        i * sizeof(uint32_t), expected, res[i]);
            }
            errors++;
        }
    }
    return errors;
}

/* ===== Test 1: CXL.cache capability + CXLQCFG via io_uring ===== */

static int test_cxl_cache_cap(struct accel_device *dev)
{
    struct accel_identify id;
    uint32_t cxlqcfg = 0;
    int ret;

    printf("Test 1: CXL.cache capability + CXLQCFG status (via io_uring)\n");

    ret = accel_identify(dev, &id);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "  FAIL: identify failed: %s\n", accel_strerror(ret));
        return -1;
    }

    printf("  Device version:  %u.%u.%u\n",
           (id.version >> 16) & 0xFFFF,
           (id.version >> 8) & 0xFF,
           id.version & 0xFF);
    printf("  io_uring fd:     %d\n", accel_get_uring_fd(dev));

    /*
     * Query CXLQCFG via io_uring ACCEL_URING_CMD_CXL_CTRL/STATUS.
     * The kernel driver enables CXL.cache in probe(), so ACTIVE should be set.
     */
    ret = accel_cxl_status(dev, &cxlqcfg);
    if (ret == ACCEL_SUCCESS) {
        printf("  CXLQCFG (io_uring): 0x%08x\n", cxlqcfg);
        printf("    EN     (bit 0):  %u  - CXL.cache queue mode enabled\n",
               cxlqcfg & 1);
        printf("    ACTIVE (bit 8):  %u  - CXL.cache protocol active\n",
               (cxlqcfg >> 8) & 1);
        printf("    MISS   (bit 9):  %u  - Last access was a miss\n",
               (cxlqcfg >> 9) & 1);
        printf("    ERR    (bit 10): %u  - Protocol error occurred\n",
               (cxlqcfg >> 10) & 1);
        printf("    LINES  [31:16]:  %u cache lines allocated\n",
               (cxlqcfg >> 16) & 0xFFFF);

        if (!((cxlqcfg >> 8) & 1)) {
            fprintf(stderr, "  WARN: CXL.cache not active; "
                    "subsequent tests use PCIe DMA\n");
        }
    } else if (ret == ACCEL_ERR_NODEV) {
        printf("  Note: CXLQCFG query returned ENODEV "
               "(base PCIe device, not CXL)\n");
    } else {
        fprintf(stderr, "  WARN: accel_cxl_status failed: %s\n",
                accel_strerror(ret));
    }

    printf("  PASS\n\n");
    return 0;
}

/* ===== Test 2 / 3: CXL.cache loopback — sync path ===== */

static int run_cxl_loopback_sync(struct accel_device *dev, uint16_t qid,
                                  size_t size, uint32_t pattern,
                                  int iterations, int *passed, int *failed)
{
    void *original = NULL;
    void *buffer = NULL;
    int i, ret = 0;

    if (posix_memalign(&original, 4096, size) != 0 ||
        posix_memalign(&buffer, 4096, size) != 0) {
        fprintf(stderr, "  FAIL: buffer allocation failed\n");
        free(original);
        return -1;
    }

    for (i = 0; i < iterations; i++) {
        fill_random_data(original, size);
        memcpy(buffer, original, size);

        /* SQE fetched via CXL.cache D2H RdOwn, CQE posted via D2H WrCurr */
        ret = accel_loopback(dev, qid, buffer, size, pattern);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "  FAIL: loopback %d: %s\n",
                    i, accel_strerror(ret));
            (*failed)++;
            continue;
        }

        if (verify_loopback_data(original, buffer, size, pattern) == 0)
            (*passed)++;
        else
            (*failed)++;
    }

    free(original);
    free(buffer);
    return 0;
}

/* ===== Test 2 / 3: CXL.cache loopback — async batch path ===== */

static int run_cxl_loopback_async_batch(struct accel_device *dev, uint16_t qid,
                                         size_t size, uint32_t pattern,
                                         int total, int batch_size,
                                         int *passed, int *failed)
{
    struct test_cxl_ctx *ctxs;
    struct accel_async_token *tokens;
    int remaining = total;
    int batch_num = 0;
    int ret = -1;

    ctxs = alloc_cxl_contexts(batch_size, size, pattern);
    if (!ctxs) {
        fprintf(stderr, "  FAIL: context allocation failed\n");
        return -1;
    }

    tokens = calloc(batch_size, sizeof(*tokens));
    if (!tokens)
        goto out;

    *passed = 0;
    *failed = 0;

    ret = accel_begin_batch(dev);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "  FAIL: begin_batch: %s\n", accel_strerror(ret));
        goto out;
    }

    while (remaining > 0) {
        int cur = (remaining < batch_size) ? remaining : batch_size;
        int i;
        uint32_t result;

        for (i = 0; i < cur; i++) {
            fill_random_data(ctxs[i].original, size);
            memcpy(ctxs[i].buffer, ctxs[i].original, size);
            ctxs[i].completed = false;

            ret = accel_async_loopback(dev, qid, ctxs[i].buffer, size,
                                       pattern, &ctxs[i].token);
            if (ret != ACCEL_SUCCESS) {
                fprintf(stderr, "  FAIL: async_loopback %d: %s\n",
                        i, accel_strerror(ret));
                accel_submit_batch(dev);
                goto out;
            }
            tokens[i] = ctxs[i].token;
        }

        ret = accel_submit_batch(dev);
        if (ret < 0) {
            fprintf(stderr, "  FAIL: submit_batch: %s\n",
                    accel_strerror(ret));
            goto out;
        }

        batch_num++;

        /* Wait for all in this batch */
        accel_wait_completions(dev, tokens, cur, cur, 5000);

        for (i = 0; i < cur; i++) {
            ret = accel_wait_completion(dev, &ctxs[i].token, &result);
            if (ret == ACCEL_SUCCESS && result == ACCEL_SUCCESS) {
                if (verify_loopback_data(ctxs[i].original, ctxs[i].buffer,
                                         size, pattern) == 0)
                    (*passed)++;
                else
                    (*failed)++;
            } else {
                (*failed)++;
            }
        }

        remaining -= cur;
        printf("\r  Batch %d: progress %d/%d",
               batch_num, total - remaining, total);
        fflush(stdout);
    }

    printf("\n");
    ret = 0;

out:
    free(tokens);
    free_cxl_contexts(ctxs, batch_size);
    return ret;
}

/* ===== Test 2 / 3: CXL.cache loopback — concurrent async path ===== */

static int run_cxl_loopback_async_concurrent(struct accel_device *dev,
                                              uint16_t qid,
                                              size_t size, uint32_t pattern,
                                              int total, int in_flight,
                                              int *passed, int *failed)
{
    struct test_cxl_ctx *ctxs;
    int submitted = 0;
    int completed_count = 0;
    int ret = -1;
    int i;

    ctxs = alloc_cxl_contexts(in_flight, size, pattern);
    if (!ctxs) {
        fprintf(stderr, "  FAIL: context allocation failed\n");
        return -1;
    }

    *passed = 0;
    *failed = 0;

    for (i = 0; i < in_flight && i < total; i++) {
        fill_random_data(ctxs[i].original, size);
        memcpy(ctxs[i].buffer, ctxs[i].original, size);
        ret = accel_async_loopback(dev, qid, ctxs[i].buffer, size,
                                   pattern, &ctxs[i].token);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "  FAIL: initial async submit %d: %s\n",
                    i, accel_strerror(ret));
            goto out;
        }
        submitted++;
    }

    ret = accel_submit_pending(dev);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "  FAIL: submit_pending: %s\n", accel_strerror(ret));
        goto out;
    }

    while (completed_count < total) {
        struct accel_async_token poll_token;
        int count;
        uint32_t result;

        count = accel_poll_completions(dev, &poll_token, 1);
        if (count < 0) {
            fprintf(stderr, "  FAIL: poll_completions: %s\n",
                    accel_strerror(count));
            goto out;
        }

        if (count > 0) {
            completed_count++;

            for (i = 0; i < in_flight; i++) {
                if (ctxs[i].token.user_data != poll_token.user_data)
                    continue;

                ret = accel_wait_completion(dev, &ctxs[i].token, &result);
                if (ret == ACCEL_SUCCESS && result == ACCEL_SUCCESS) {
                    if (verify_loopback_data(ctxs[i].original, ctxs[i].buffer,
                                             size, pattern) == 0)
                        (*passed)++;
                    else
                        (*failed)++;
                } else {
                    (*failed)++;
                }

                if (submitted < total) {
                    fill_random_data(ctxs[i].original, size);
                    memcpy(ctxs[i].buffer, ctxs[i].original, size);
                    ret = accel_async_loopback(dev, qid, ctxs[i].buffer, size,
                                               pattern, &ctxs[i].token);
                    if (ret == ACCEL_SUCCESS) {
                        accel_submit_pending(dev);
                        submitted++;
                    }
                }
                break;
            }

            printf("\r  Progress: %d/%d (in-flight: %d)",
                   completed_count, total, submitted - completed_count);
            fflush(stdout);
        } else {
            accel_wait_completions(dev, &poll_token, 1, 1, 10);
        }
    }

    printf("\n");
    ret = 0;

out:
    free_cxl_contexts(ctxs, in_flight);
    return ret;
}

/* ===== Test 4: CXL.cache mem write/read — sync path ===== */

static int run_cxl_mem_sync(struct accel_device *dev, uint16_t qid,
                              size_t size, int iterations,
                              int *passed, int *failed)
{
    void *write_buf = NULL;
    void *read_buf = NULL;
    int i, ret;

    if (posix_memalign(&write_buf, 4096, size) != 0 ||
        posix_memalign(&read_buf, 4096, size) != 0) {
        fprintf(stderr, "  FAIL: buffer allocation failed\n");
        free(write_buf);
        return -1;
    }

    for (i = 0; i < iterations; i++) {
        fill_random_data(write_buf, size);
        memset(read_buf, 0, size);

        /* MEM_WRITE SQE fetched via CXL.cache D2H RdOwn */
        ret = accel_mem_write(dev, qid, write_buf, 0, size);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "  FAIL: mem_write %d: %s\n",
                    i, accel_strerror(ret));
            (*failed)++;
            continue;
        }

        /* MEM_READ SQE fetched via CXL.cache D2H RdOwn */
        ret = accel_mem_read(dev, qid, read_buf, 0, size);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "  FAIL: mem_read %d: %s\n",
                    i, accel_strerror(ret));
            (*failed)++;
            continue;
        }

        if (memcmp(write_buf, read_buf, size) == 0)
            (*passed)++;
        else {
            fprintf(stderr, "  FAIL: iteration %d: data mismatch\n", i);
            (*failed)++;
        }
    }

    free(write_buf);
    free(read_buf);
    return 0;
}

/* ===== Test 4: CXL.cache mem write/read — async batch path =====
 *
 * Strategy: submit all writes as a batch, wait for all, then submit
 * all reads as a batch, wait for all, then verify.
 */
static int run_cxl_mem_async_batch(struct accel_device *dev, uint16_t qid,
                                    size_t size, int iterations, int batch_size,
                                    int *passed, int *failed)
{
    struct test_cxl_ctx *ctxs;
    struct accel_async_token *tokens;
    int remaining = iterations;
    int batch_num = 0;
    int ret = -1;

    ctxs = alloc_cxl_contexts(batch_size, size, 0);
    if (!ctxs) {
        fprintf(stderr, "  FAIL: context allocation failed\n");
        return -1;
    }

    tokens = calloc(batch_size, sizeof(*tokens));
    if (!tokens)
        goto out;

    *passed = 0;
    *failed = 0;

    while (remaining > 0) {
        int cur = (remaining < batch_size) ? remaining : batch_size;
        int i;
        uint32_t result;

        /* Prepare write data for this batch */
        for (i = 0; i < cur; i++) {
            fill_random_data(ctxs[i].original, size);
            memset(ctxs[i].buffer, 0, size);
        }

        /* Batch: submit all writes */
        ret = accel_begin_batch(dev);
        if (ret != ACCEL_SUCCESS)
            goto out;

        for (i = 0; i < cur; i++) {
            ret = accel_async_mem_write(dev, qid, ctxs[i].original,
                                        (uint64_t)i * size, size,
                                        &ctxs[i].token);
            if (ret != ACCEL_SUCCESS) {
                accel_submit_batch(dev);
                goto out;
            }
            tokens[i] = ctxs[i].token;
        }

        ret = accel_submit_batch(dev);
        if (ret < 0)
            goto out;

        accel_wait_completions(dev, tokens, cur, cur, 5000);

        for (i = 0; i < cur; i++)
            accel_wait_completion(dev, &ctxs[i].token, &result);

        /* Batch: submit all reads */
        ret = accel_begin_batch(dev);
        if (ret != ACCEL_SUCCESS)
            goto out;

        for (i = 0; i < cur; i++) {
            ret = accel_async_mem_read(dev, qid, ctxs[i].buffer,
                                       (uint64_t)i * size, size,
                                       &ctxs[i].token);
            if (ret != ACCEL_SUCCESS) {
                accel_submit_batch(dev);
                goto out;
            }
            tokens[i] = ctxs[i].token;
        }

        ret = accel_submit_batch(dev);
        if (ret < 0)
            goto out;

        accel_wait_completions(dev, tokens, cur, cur, 5000);

        for (i = 0; i < cur; i++) {
            accel_wait_completion(dev, &ctxs[i].token, &result);
            if (memcmp(ctxs[i].original, ctxs[i].buffer, size) == 0)
                (*passed)++;
            else {
                fprintf(stderr, "  FAIL: batch item %d: mismatch\n", i);
                (*failed)++;
            }
        }

        remaining -= cur;
        batch_num++;
        printf("\r  Batch %d: progress %d/%d",
               batch_num, iterations - remaining, iterations);
        fflush(stdout);
    }

    printf("\n");
    ret = 0;

out:
    free(tokens);
    free_cxl_contexts(ctxs, batch_size);
    return ret;
}

/* ===== Test wrappers ===== */

static int test_cxl_loopback(struct accel_device *dev, uint16_t qid,
                              size_t size, int iterations,
                              bool async_mode, bool concurrent_mode,
                              int batch_size)
{
    int passed = 0, failed = 0;
    int ret;

    printf("Test 2: CXL.cache loopback (SQ D2H RdOwn + CQ D2H WrCurr)\n");
    printf("  Size: %zu bytes, iterations: %d, mode: %s\n",
           size, iterations,
           concurrent_mode ? "async-concurrent" :
           async_mode ? "async-batch" : "sync");

    if (concurrent_mode) {
        ret = run_cxl_loopback_async_concurrent(dev, qid, size, 0,
                                                 iterations, batch_size,
                                                 &passed, &failed);
    } else if (async_mode) {
        ret = run_cxl_loopback_async_batch(dev, qid, size, 0, iterations,
                                            batch_size, &passed, &failed);
    } else {
        ret = run_cxl_loopback_sync(dev, qid, size, 0, iterations,
                                     &passed, &failed);
    }

    if (ret < 0)
        return ret;

    printf("  Results: %d passed, %d failed\n", passed, failed);
    printf("  %s\n\n", failed == 0 ? "PASS" : "FAIL");
    return failed > 0 ? -1 : 0;
}

static int test_cxl_loopback_xor(struct accel_device *dev, uint16_t qid,
                                   size_t size, int iterations,
                                   bool async_mode, bool concurrent_mode,
                                   int batch_size)
{
    const uint32_t pattern = 0xDEADBEEF;
    int passed = 0, failed = 0;
    int ret;

    printf("Test 3: CXL.cache loopback with XOR (SQ + CQ via CXL.cache)\n");
    printf("  Size: %zu bytes, pattern: 0x%08x, iterations: %d, mode: %s\n",
           size, pattern, iterations,
           concurrent_mode ? "async-concurrent" :
           async_mode ? "async-batch" : "sync");

    if (concurrent_mode) {
        ret = run_cxl_loopback_async_concurrent(dev, qid, size, pattern,
                                                 iterations, batch_size,
                                                 &passed, &failed);
    } else if (async_mode) {
        ret = run_cxl_loopback_async_batch(dev, qid, size, pattern, iterations,
                                            batch_size, &passed, &failed);
    } else {
        ret = run_cxl_loopback_sync(dev, qid, size, pattern, iterations,
                                     &passed, &failed);
    }

    if (ret < 0)
        return ret;

    printf("  Results: %d passed, %d failed\n", passed, failed);
    printf("  %s\n\n", failed == 0 ? "PASS" : "FAIL");
    return failed > 0 ? -1 : 0;
}

static int test_cxl_mem_write_read(struct accel_device *dev, uint16_t qid,
                                    size_t size, int iterations,
                                    bool async_mode, bool concurrent_mode,
                                    int batch_size)
{
    int passed = 0, failed = 0;
    int ret;

    printf("Test 4: CXL.cache memory write/read (SQ + CQ via CXL.cache)\n");
    printf("  Size: %zu bytes, iterations: %d, mode: %s\n",
           size, iterations,
           concurrent_mode ? "async-batch" :
           async_mode ? "async-batch" : "sync");

    if (async_mode || concurrent_mode) {
        /* Both batch and concurrent use the batch write-then-read strategy */
        ret = run_cxl_mem_async_batch(dev, qid, size, iterations, batch_size,
                                       &passed, &failed);
    } else {
        ret = run_cxl_mem_sync(dev, qid, size, iterations, &passed, &failed);
    }

    if (ret < 0)
        return ret;

    printf("  Results: %d passed, %d failed\n", passed, failed);
    printf("  %s\n\n", failed == 0 ? "PASS" : "FAIL");
    return failed > 0 ? -1 : 0;
}

/* ===== Usage and main ===== */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("\nOptions:\n");
    printf("  -d <device>     Device path (default: %s)\n", DEFAULT_DEVICE);
    printf("  -s <size>       Transfer size in bytes (default: %d)\n",
           DEFAULT_SIZE);
    printf("  -n <count>      Iterations per test (default: %d)\n",
           DEFAULT_ITERATIONS);
    printf("  -q <qid>        Queue ID to use (default: 1)\n");
    printf("  -a              Async batch mode (io_uring batch submission)\n");
    printf("  -b <size>       Batch / in-flight size (default: %d)\n",
           DEFAULT_BATCH_SIZE);
    printf("  -c              Concurrent async mode (keep N ops in flight)\n");
    printf("  -h              Show this help\n");
    printf("\nTests:\n");
    printf("  1. CXL.cache capability + CXLQCFG status via io_uring\n");
    printf("  2. Loopback via CXL.cache SQ/CQ (sync/async)\n");
    printf("  3. Loopback with XOR via CXL.cache SQ/CQ (sync/async)\n");
    printf("  4. Memory write/read via CXL.cache SQ/CQ (sync/async)\n");
    printf("\nCXL.cache queue mode:\n");
    printf("  SQ: SQE fetch via D2H RdOwn (64B cache-line granularity)\n");
    printf("  CQ: CQE post via D2H WrCurr (read-modify-write for 16B CQEs)\n");
    printf("  Enable: kernel writes CXLQCFG.EN=1 during probe\n");
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    size_t size = DEFAULT_SIZE;
    int iterations = DEFAULT_ITERATIONS;
    int batch_size = DEFAULT_BATCH_SIZE;
    uint16_t qid = 1;
    bool async_mode = false;
    bool concurrent_mode = false;
    struct accel_device *dev;
    struct accel_stats stats_before, stats_after;
    int opt;
    int ret;
    int total_pass = 0;
    int total_fail = 0;

    while ((opt = getopt(argc, argv, "d:s:n:q:ab:ch")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 's': size = (size_t)atoi(optarg); break;
        case 'n': iterations = atoi(optarg); break;
        case 'q': qid = (uint16_t)atoi(optarg); break;
        case 'a': async_mode = true; break;
        case 'b': batch_size = atoi(optarg); break;
        case 'c': concurrent_mode = true; break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    printf("PCIe Accelerator CXL Type 1 Test (io_uring)\n");
    printf("============================================\n");
    printf("Device:     %s\n", device);
    printf("Size:       %zu bytes\n", size);
    printf("Iterations: %d\n", iterations);
    printf("Queue ID:   %u\n", qid);
    printf("Mode:       %s\n",
           concurrent_mode ? "async (concurrent)" :
           async_mode ? "async (batch)" : "synchronous");
    if (async_mode || concurrent_mode)
        printf("Batch/Flight: %d\n", batch_size);
    printf("\n");

    dev = accel_open(device);
    if (!dev) {
        fprintf(stderr, "Failed to open device %s: %s\n",
                device, strerror(errno));
        return 1;
    }

    /* Test 1: Capability check + CXLQCFG query via io_uring */
    ret = test_cxl_cache_cap(dev);
    if (ret == 0) total_pass++; else total_fail++;

    /* Create I/O queue */
    ret = accel_create_queue(dev, qid);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to create queue %u: %s\n",
                qid, accel_strerror(ret));
        accel_close(dev);
        return 1;
    }

    srand(time(NULL));

    accel_get_stats(dev, &stats_before);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    /* Test 2: Loopback */
    ret = test_cxl_loopback(dev, qid, size, iterations,
                             async_mode, concurrent_mode, batch_size);
    if (ret == 0) total_pass++; else total_fail++;

    /* Test 3: Loopback with XOR */
    ret = test_cxl_loopback_xor(dev, qid, size, iterations,
                                  async_mode, concurrent_mode, batch_size);
    if (ret == 0) total_pass++; else total_fail++;

    /* Test 4: Memory write/read */
    ret = test_cxl_mem_write_read(dev, qid, size, iterations,
                                   async_mode, concurrent_mode, batch_size);
    if (ret == 0) total_pass++; else total_fail++;

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    accel_get_stats(dev, &stats_after);

    printf("============================================\n");
    printf("CXL Test Summary:\n");
    printf("  Tests passed:         %d\n", total_pass);
    printf("  Tests failed:         %d\n", total_fail);
    printf("  Total time:           %.3f seconds\n", elapsed);
    printf("  io_uring submissions: %lu\n",
           stats_after.uring_submissions - stats_before.uring_submissions);
    printf("  io_uring completions: %lu\n",
           stats_after.uring_completions - stats_before.uring_completions);
    printf("  Result:               %s\n",
           total_fail == 0 ? "ALL PASSED" : "SOME FAILED");
    printf("\n");

    accel_delete_queue(dev, qid);
    accel_close(dev);

    return total_fail > 0 ? 1 : 0;
}
