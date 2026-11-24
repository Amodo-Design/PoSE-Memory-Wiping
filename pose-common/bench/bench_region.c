/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * bench_region — labels N full POSE_CHUNK_BLOCKS chunks and reports throughput.
 *
 * This is the representative workload for profiling: each chunk allocates a
 * ~17 MB scratch label buffer and traverses the full depth-robust graph DAG.
 * The butterfly access pattern creates heavy pressure on the L2 cache / DRAM.
 *
 * Usage:
 *   bench_region [chunks]     default: 4 chunks (4 × 128 KB = 512 KB labeled)
 *
 * Valgrind cachegrind:
 *   valgrind --tool=cachegrind --cachegrind-out-file=cg.out ./bench_region 1
 *   cg_annotate cg.out --auto=yes | head -80
 *
 * Valgrind callgrind (call-graph profiling):
 *   valgrind --tool=callgrind --callgrind-out-file=cl.out ./bench_region 1
 *   callgrind_annotate cl.out --auto=yes | head -120
 */
#include "../include/graph.h"
#include "../include/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    int chunks = (argc > 1) ? atoi(argv[1]) : 4;
    if (chunks <= 0) chunks = 1;

    const uint64_t blocks_per_chunk = POSE_CHUNK_BLOCKS;
    const uint64_t total_blocks     = (uint64_t)chunks * blocks_per_chunk;
    const size_t   region_bytes     = total_blocks * POSE_HASH_BYTES;

    uint8_t seed[POSE_HASH_BYTES] = {
        0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe, 0xba, 0xbe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
        0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
    };

    uint8_t *region = (uint8_t *)malloc(region_bytes);
    if (!region) {
        fprintf(stderr, "malloc failed (%zu bytes)\n", region_bytes);
        return 1;
    }

    printf("bench_region: %d chunk(s) × %llu blocks = %llu blocks = %.1f MB output\n",
           chunks,
           (unsigned long long)blocks_per_chunk,
           (unsigned long long)total_blocks,
           (double)region_bytes / (1024.0 * 1024.0));
    printf("  POSE_CHUNK_BLOCKS = %d\n", POSE_CHUNK_BLOCKS);
    printf("  POSE_HASH_BYTES   = %d\n", POSE_HASH_BYTES);
    fflush(stdout);

#ifdef BENCH_ALGO_SHA256
    pose_hash_algo_t algo = POSE_HASH_SHA256;
    const char *algo_name = "sha256";
#elif defined(BENCH_ALGO_AES_PRF)
    pose_hash_algo_t algo = POSE_HASH_AES_PRF;
    const char *algo_name = "aes_prf";
#else
    pose_hash_algo_t algo = POSE_HASH_BLAKE3;
    const char *algo_name = "blake3";
#endif

    printf("  algo = %s\n", algo_name);
    fflush(stdout);

    double t0 = now_secs();
    int rc = pose_graph_label_region(seed, POSE_HASH_BYTES,
                                     region, region_bytes, 0, algo, POSE_CHUNK_BLOCKS);
    double elapsed = now_secs() - t0;

    if (rc != 0) {
        fprintf(stderr, "pose_graph_label_region failed (rc=%d)\n", rc);
        free(region);
        return 1;
    }

    double mb       = (double)region_bytes / (1024.0 * 1024.0);
    double mb_per_s = mb / elapsed;

    printf("  elapsed:    %.3f s\n", elapsed);
    printf("  throughput: %.2f MB/s\n", mb_per_s);

    /* Touch the output so the compiler can't eliminate the work. */
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < region_bytes; i += 4096)
        sink ^= region[i];
    (void)sink;

    free(region);
    return 0;
}
