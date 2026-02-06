/* SPDX-License-Identifier: MIT */
/*
 * PCIe Accelerator Device - Loopback Test with io_uring
 *
 * Copyright (C) 2026
 *
 * This test program verifies basic device functionality using the
 * loopback command. Supports both synchronous and asynchronous modes.
 *
 * Async mode uses io_uring for high-performance batched operations.
 *
 * Usage: test_loopback [options]
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
#define DEFAULT_PATTERN     0
#define DEFAULT_ITERATIONS  100
#define DEFAULT_BATCH_SIZE  32

/**
 * Test context for async operations
 */
struct test_context {
    void *original;
    void *buffer;
    size_t size;
    uint32_t pattern;
    int iteration;
    struct accel_async_token token;
    bool completed;
    int result;
};

/**
 * fill_random_data - Fill buffer with pseudo-random data
 */
static void fill_random_data(void *buf, size_t len)
{
    uint32_t *data = buf;
    size_t count = len / sizeof(uint32_t);

    for (size_t i = 0; i < count; i++)
        data[i] = rand();

    /* Handle remaining bytes */
    uint8_t *tail = (uint8_t *)(data + count);
    size_t remaining = len % sizeof(uint32_t);
    for (size_t i = 0; i < remaining; i++)
        tail[i] = rand() & 0xFF;
}

/**
 * verify_loopback_data - Verify loopback result
 */
static int verify_loopback_data(const void *original, const void *result,
                                size_t len, uint32_t pattern)
{
    const uint32_t *orig = original;
    const uint32_t *res = result;
    size_t count = len / sizeof(uint32_t);
    int errors = 0;

    for (size_t i = 0; i < count; i++) {
        uint32_t expected = orig[i] ^ pattern;
        if (res[i] != expected) {
            if (errors < 10) {
                fprintf(stderr, "Mismatch at offset %zu: "
                        "expected 0x%08x, got 0x%08x\n",
                        i * sizeof(uint32_t), expected, res[i]);
            }
            errors++;
        }
    }

    return errors;
}

/**
 * run_loopback_sync - Run a single synchronous loopback test
 */
static int run_loopback_sync(struct accel_device *dev, uint16_t qid,
                             size_t size, uint32_t pattern)
{
    void *original = NULL;
    void *buffer = NULL;
    int ret = -1;

    /* Allocate aligned buffers */
    if (posix_memalign(&original, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate original buffer\n");
        return -1;
    }

    if (posix_memalign(&buffer, 4096, size) != 0) {
        fprintf(stderr, "Failed to allocate work buffer\n");
        goto out;
    }

    /* Fill with random data */
    fill_random_data(original, size);
    memcpy(buffer, original, size);

    /* Perform loopback (synchronous) */
    ret = accel_loopback(dev, qid, buffer, size, pattern);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Loopback failed: %s\n", accel_strerror(ret));
        ret = -1;
        goto out;
    }

    /* Verify data */
    int errors = verify_loopback_data(original, buffer, size, pattern);
    if (errors > 0) {
        fprintf(stderr, "Data verification failed: %d errors\n", errors);
        ret = -1;
        goto out;
    }

    ret = 0;

out:
    free(original);
    free(buffer);
    return ret;
}

/**
 * alloc_test_contexts - Allocate test contexts for async operations
 */
static struct test_context *alloc_test_contexts(int count, size_t size,
                                                uint32_t pattern)
{
    struct test_context *contexts;

    contexts = calloc(count, sizeof(*contexts));
    if (!contexts)
        return NULL;

    for (int i = 0; i < count; i++) {
        contexts[i].size = size;
        contexts[i].pattern = pattern;
        contexts[i].iteration = i;
        contexts[i].completed = false;
        contexts[i].result = -1;

        if (posix_memalign(&contexts[i].original, 4096, size) != 0)
            goto cleanup;

        if (posix_memalign(&contexts[i].buffer, 4096, size) != 0)
            goto cleanup;

        fill_random_data(contexts[i].original, size);
        memcpy(contexts[i].buffer, contexts[i].original, size);
    }

    return contexts;

cleanup:
    for (int i = 0; i < count; i++) {
        free(contexts[i].original);
        free(contexts[i].buffer);
    }
    free(contexts);
    return NULL;
}

/**
 * free_test_contexts - Free test contexts
 */
static void free_test_contexts(struct test_context *contexts, int count)
{
    if (!contexts)
        return;

    for (int i = 0; i < count; i++) {
        free(contexts[i].original);
        free(contexts[i].buffer);
    }
    free(contexts);
}

/**
 * run_loopback_async_batch - Run async loopback tests in batches
 *
 * This demonstrates io_uring batch submission for maximum throughput.
 */
static int run_loopback_async_batch(struct accel_device *dev, uint16_t qid,
                                    size_t size, uint32_t pattern,
                                    int total_iterations, int batch_size,
                                    int *passed, int *failed)
{
    struct test_context *contexts = NULL;
    struct accel_async_token *tokens = NULL;
    int ret = -1;
    int remaining = total_iterations;
    int batch_num = 0;

    contexts = alloc_test_contexts(batch_size, size, pattern);
    if (!contexts) {
        fprintf(stderr, "Failed to allocate test contexts\n");
        return -1;
    }

    tokens = calloc(batch_size, sizeof(*tokens));
    if (!tokens)
        goto out;

    *passed = 0;
    *failed = 0;

    printf("Using io_uring async batch submission (batch size: %d)\n\n",
           batch_size);

    /* Begin batch mode for better performance */
    ret = accel_begin_batch(dev);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to begin batch mode: %s\n",
                accel_strerror(ret));
        goto out;
    }

    while (remaining > 0) {
        int current_batch = (remaining < batch_size) ? remaining : batch_size;

        /* Prepare and submit batch */
        for (int i = 0; i < current_batch; i++) {
            /* Re-initialize buffer for this iteration */
            fill_random_data(contexts[i].original, size);
            memcpy(contexts[i].buffer, contexts[i].original, size);
            contexts[i].iteration = total_iterations - remaining + i;
            contexts[i].completed = false;

            /* Queue async operation (batched, not submitted yet) */
            ret = accel_async_loopback(dev, qid, contexts[i].buffer, size,
                                       pattern, &contexts[i].token);
            if (ret != ACCEL_SUCCESS) {
                fprintf(stderr, "Failed to queue async loopback %d: %s\n",
                        contexts[i].iteration, accel_strerror(ret));
                /* Submit what we have so far */
                if (i > 0)
                    accel_submit_batch(dev);
                goto out;
            }

            tokens[i] = contexts[i].token;
        }

        /* Submit the entire batch via io_uring */
        ret = accel_submit_batch(dev);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to submit batch: %s\n",
                    accel_strerror(ret));
            goto out;
        }

        batch_num++;
        printf("  Batch %d: submitted %d operations\n", batch_num, current_batch);

        /* Wait for all completions in this batch */
        int completed = accel_wait_completions(dev, tokens, current_batch,
                                               current_batch, 5000);
        if (completed < 0) {
            fprintf(stderr, "Failed to wait for completions: %s\n",
                    accel_strerror(completed));
            goto out;
        }

        printf("  Batch %d: completed %d operations\n", batch_num, completed);

        /* Process results - wait for each token individually to get result */
        for (int i = 0; i < current_batch; i++) {
            uint32_t result;
            ret = accel_wait_completion(dev, &contexts[i].token, &result);
            contexts[i].completed = true;

            if (ret == ACCEL_SUCCESS && result == ACCEL_SUCCESS) {
                /* Verify data */
                int errors = verify_loopback_data(
                    contexts[i].original,
                    contexts[i].buffer,
                    contexts[i].size,
                    contexts[i].pattern);

                if (errors == 0) {
                    (*passed)++;
                    contexts[i].result = 0;
                } else {
                    (*failed)++;
                    contexts[i].result = -1;
                }
            } else {
                (*failed)++;
                contexts[i].result = -1;
            }
        }

        remaining -= current_batch;
        printf("\rProgress: %d/%d", total_iterations - remaining, total_iterations);
        fflush(stdout);
    }

    printf("\n");
    ret = 0;

out:
    free(tokens);
    free_test_contexts(contexts, batch_size);
    return ret;
}

/**
 * run_loopback_async_concurrent - Run multiple async operations concurrently
 *
 * Demonstrates overlapping I/O using io_uring with poll-based completion.
 */
static int run_loopback_async_concurrent(struct accel_device *dev, uint16_t qid,
                                         size_t size, uint32_t pattern,
                                         int total_iterations, int in_flight,
                                         int *passed, int *failed)
{
    struct test_context *contexts = NULL;
    int ret = -1;
    int submitted = 0;
    int completed_count = 0;

    contexts = alloc_test_contexts(in_flight, size, pattern);
    if (!contexts) {
        fprintf(stderr, "Failed to allocate test contexts\n");
        return -1;
    }

    *passed = 0;
    *failed = 0;

    printf("Using io_uring async with %d ops in flight\n\n", in_flight);

    /* Initial submission wave */
    for (int i = 0; i < in_flight && submitted < total_iterations; i++) {
        ret = accel_async_loopback(dev, qid, contexts[i].buffer, size,
                                   pattern, &contexts[i].token);
        if (ret != ACCEL_SUCCESS) {
            fprintf(stderr, "Failed to submit initial async op %d: %s\n",
                    i, accel_strerror(ret));
            goto out;
        }
        submitted++;
    }

    /* Submit all pending operations */
    ret = accel_submit_pending(dev);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to submit pending ops: %s\n",
                accel_strerror(ret));
        goto out;
    }

    /* Process completions and submit new operations */
    while (completed_count < total_iterations) {
        struct accel_async_token poll_token;
        int count;

        /* Poll for completions (non-blocking) */
        count = accel_poll_completions(dev, &poll_token, 1);
        if (count < 0) {
            fprintf(stderr, "Failed to poll completions: %s\n",
                    accel_strerror(count));
            goto out;
        }

        if (count > 0) {
            completed_count++;

            /* Find and process the completed context */
            for (int i = 0; i < in_flight; i++) {
                if (contexts[i].token.user_data == poll_token.user_data) {
                    /* Get the result for this token */
                    uint32_t result;
                    ret = accel_wait_completion(dev, &contexts[i].token, &result);

                    if (ret == ACCEL_SUCCESS && result == ACCEL_SUCCESS) {
                        int errors = verify_loopback_data(
                            contexts[i].original,
                            contexts[i].buffer,
                            size, pattern);

                        if (errors == 0) {
                            (*passed)++;
                        } else {
                            (*failed)++;
                        }
                    } else {
                        (*failed)++;
                    }

                    /* Submit another operation if we have more work */
                    if (submitted < total_iterations) {
                        /* Reinitialize this context */
                        fill_random_data(contexts[i].original, size);
                        memcpy(contexts[i].buffer, contexts[i].original, size);

                        ret = accel_async_loopback(dev, qid,
                                                   contexts[i].buffer, size,
                                                   pattern, &contexts[i].token);
                        if (ret == ACCEL_SUCCESS) {
                            accel_submit_pending(dev);
                            submitted++;
                        }
                    }
                    break;
                }
            }

            printf("\rProgress: %d/%d (in-flight: %d)",
                   completed_count, total_iterations,
                   submitted - completed_count);
            fflush(stdout);
        } else {
            /* No completion ready, wait a bit for any completion */
            count = accel_wait_completions(dev, &poll_token, 1, 1, 10);
            if (count > 0) {
                /* Found a completion, re-process in next iteration */
                continue;
            }
        }
    }

    printf("\n");
    ret = 0;

out:
    free_test_contexts(contexts, in_flight);
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
    printf("  -p <pattern>    XOR pattern (default: 0x%x)\n", DEFAULT_PATTERN);
    printf("  -n <count>      Number of iterations (default: %d)\n", DEFAULT_ITERATIONS);
    printf("  -q <qid>        Queue ID to use (default: 1)\n");
    printf("  -a              Use async mode with io_uring\n");
    printf("  -b <size>       Batch size for async mode (default: %d)\n", DEFAULT_BATCH_SIZE);
    printf("  -c              Use concurrent async mode (poll-based)\n");
    printf("  -h              Show this help\n");
    printf("\nAsync Modes:\n");
    printf("  -a              Batch mode: submit batches, wait for all\n");
    printf("  -c              Concurrent mode: keep N ops in flight\n");
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_DEVICE;
    size_t size = DEFAULT_SIZE;
    uint32_t pattern = DEFAULT_PATTERN;
    int iterations = DEFAULT_ITERATIONS;
    int batch_size = DEFAULT_BATCH_SIZE;
    uint16_t qid = 1;
    bool async_mode = false;
    bool concurrent_mode = false;
    struct accel_device *dev;
    struct accel_identify id;
    struct accel_stats stats_before, stats_after;
    int opt;
    int ret;

    /* Parse arguments */
    while ((opt = getopt(argc, argv, "d:s:p:n:q:ab:ch")) != -1) {
        switch (opt) {
        case 'd':
            device = optarg;
            break;
        case 's':
            size = atoi(optarg);
            break;
        case 'p':
            pattern = strtoul(optarg, NULL, 0);
            break;
        case 'n':
            iterations = atoi(optarg);
            break;
        case 'q':
            qid = atoi(optarg);
            break;
        case 'a':
            async_mode = true;
            break;
        case 'b':
            batch_size = atoi(optarg);
            break;
        case 'c':
            concurrent_mode = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    printf("PCIe Accelerator Loopback Test (io_uring)\n");
    printf("=========================================\n");
    printf("Device:     %s\n", device);
    printf("Size:       %zu bytes\n", size);
    printf("Pattern:    0x%08x\n", pattern);
    printf("Iterations: %d\n", iterations);
    printf("Queue ID:   %u\n", qid);
    printf("Mode:       %s\n",
           concurrent_mode ? "async (concurrent)" :
           async_mode ? "async (batch)" : "synchronous");
    if (async_mode || concurrent_mode)
        printf("Batch/Flight: %d\n", batch_size);
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
    printf("  io_uring fd:  %d\n", accel_get_uring_fd(dev));
    printf("\n");

    /* Create I/O queue */
    ret = accel_create_queue(dev, qid, 256, 256);
    if (ret != ACCEL_SUCCESS) {
        fprintf(stderr, "Failed to create queue %u: %s\n",
                qid, accel_strerror(ret));
        accel_close(dev);
        return 1;
    }

    /* Get stats before test */
    accel_get_stats(dev, &stats_before);

    /* Seed random number generator */
    srand(time(NULL));

    /* Run test iterations */
    printf("Running %d loopback iterations...\n", iterations);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    int passed = 0;
    int failed = 0;

    if (concurrent_mode) {
        /* Concurrent async mode with polling */
        ret = run_loopback_async_concurrent(dev, qid, size, pattern,
                                            iterations, batch_size,
                                            &passed, &failed);
        if (ret != 0) {
            fprintf(stderr, "Concurrent test failed\n");
        }
    } else if (async_mode) {
        /* Batch async mode */
        ret = run_loopback_async_batch(dev, qid, size, pattern,
                                       iterations, batch_size,
                                       &passed, &failed);
        if (ret != 0) {
            fprintf(stderr, "Batch async test failed\n");
        }
    } else {
        /* Synchronous mode */
        for (int i = 0; i < iterations; i++) {
            if (run_loopback_sync(dev, qid, size, pattern) == 0) {
                passed++;
            } else {
                failed++;
            }

            /* Progress indicator */
            if ((i + 1) % 10 == 0) {
                printf("\rProgress: %d/%d", i + 1, iterations);
                fflush(stdout);
            }
        }
        printf("\n");
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    printf("\n");

    /* Calculate elapsed time */
    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    /* Get stats after test */
    accel_get_stats(dev, &stats_after);

    /* Print results */
    printf("Results:\n");
    printf("  Passed:       %d\n", passed);
    printf("  Failed:       %d\n", failed);
    printf("  Total time:   %.3f seconds\n", elapsed);
    printf("  Throughput:   %.2f MB/s\n",
           (passed * size / (1024.0 * 1024.0)) / elapsed);
    printf("  IOPS:         %.0f\n", passed / elapsed);
    printf("\n");

    printf("Statistics:\n");
    printf("  Commands submitted: %lu (+%lu)\n",
           stats_after.cmd_submitted,
           stats_after.cmd_submitted - stats_before.cmd_submitted);
    printf("  Commands completed: %lu (+%lu)\n",
           stats_after.cmd_completed,
           stats_after.cmd_completed - stats_before.cmd_completed);
    printf("  io_uring submissions: %lu\n", stats_after.uring_submissions);
    printf("  io_uring completions: %lu\n", stats_after.uring_completions);
    printf("\n");

    /* Cleanup */
    accel_delete_queue(dev, qid);
    accel_close(dev);

    return failed > 0 ? 1 : 0;
}
