/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * sweep_parity.c — confirm the faithful in-place prover's persisted output set
 * is byte-identical to the do_label ground truth (via pose_graph_challenge)
 * across a wide range of m, and that the bounded arena assert never trips.
 *
 * Build:
 *   cc -O2 -I common/include common/test/sweep_parity.c \
 *      common/build/libpose_common.a -lpthread -o /tmp/sweep_parity
 */
#include "graph.h"
#include "hash.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t SEED[POSE_HASH_BYTES] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
    0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
    0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
};

static int check_one(uint64_t m)
{
    uint32_t cb = (uint32_t)m;
    uint64_t super = pose_graph_super_chunk_blocks(cb);
    if (super != m) {
        fprintf(stderr, "m=%" PRIu64 ": super_chunk_blocks=%" PRIu64 " != m\n", m, super);
        return -1;
    }
    size_t super_bytes = (size_t)super * POSE_HASH_BYTES;

    uint8_t *buf = (uint8_t *)calloc(1, super_bytes);
    uint64_t *node_ids = (uint64_t *)malloc((size_t)cb * sizeof(uint64_t));
    size_t scratch_sz = pose_graph_scratch_bytes_inplace(cb);
    uint8_t *scratch[POSE_REGION_THREADS];
    for (int i = 0; i < POSE_REGION_THREADS; i++)
        scratch[i] = (uint8_t *)malloc(scratch_sz);
    if (!buf || !node_ids) { fprintf(stderr, "alloc fail m=%" PRIu64 "\n", m); return -1; }

    pose_graph_challenge_node_ids(cb, node_ids, scratch[0]);

    pose_graph_pool_t *pool = pose_graph_pool_create();
    int rc = pose_graph_label_region_pooled_inplace(
                 SEED, POSE_HASH_BYTES, buf, super_bytes, 0,
                 scratch, pool, POSE_HASH_BLAKE3, cb);
    pose_graph_pool_destroy(pool);
    if (rc != 0) { fprintf(stderr, "label rc=%d m=%" PRIu64 "\n", rc, m); return -1; }

    uint8_t cseed[POSE_HASH_BYTES];
    pose_chunk_seed(cseed, SEED, POSE_HASH_BYTES, 0);

    /* Ground truth: one do_label producing all m output labels in rank order. */
    uint8_t *expected = (uint8_t *)malloc((size_t)cb * POSE_HASH_BYTES);
    if (!expected || pose_graph_label(cseed, POSE_HASH_BYTES, cb, expected,
                                      POSE_HASH_BLAKE3) != 0) {
        fprintf(stderr, "pose_graph_label failed m=%" PRIu64 "\n", m);
        return -1;
    }

    int bad = 0;
    for (uint32_t r = 0; r < cb && bad == 0; r++) {
        if (node_ids[r] != r) { fprintf(stderr, "m=%" PRIu64 " r=%u node_id!=r\n", m, r); bad = 1; }
        if (memcmp(expected + (size_t)r * POSE_HASH_BYTES,
                   buf + (size_t)r * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0) {
            fprintf(stderr, "m=%" PRIu64 " rank %u MISMATCH\n", m, r);
            bad = 1;
        }
    }

    free(expected);
    free(buf); free(node_ids);
    for (int i = 0; i < POSE_REGION_THREADS; i++) free(scratch[i]);
    if (!bad)
        printf("m=%-8" PRIu64 " OK  (scratch %zu KB/thread)\n", m, scratch_sz / 1024);
    return bad ? -1 : 0;
}

int main(void)
{
    int rc = 0;
    for (int e = 1; e <= 16; e++)
        if (check_one((uint64_t)1 << e) != 0) rc = 1;
    /* non-power-of-two sizes exercise retained = m - half_w */
    uint64_t odd[] = { 3, 5, 7, 17, 100, 1000, 4095, 4097, 50000 };
    for (size_t i = 0; i < sizeof(odd)/sizeof(odd[0]); i++)
        if (check_one(odd[i]) != 0) rc = 1;
    printf(rc ? "\nFAILURES\n" : "\nAll m parity-checked\n");
    return rc;
}
