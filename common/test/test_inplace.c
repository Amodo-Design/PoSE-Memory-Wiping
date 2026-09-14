/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * Validation suite for pose_graph_label_region_pooled_inplace (paper-faithful:
 * the persisted super-chunk holds ONLY the output set O(G), in rank order).
 *
 * Properties under test:
 *
 *  1. CORRECTNESS  — for every challenge rank r, the 32 bytes at
 *                    buf[node_ids[r] * 32] match pose_graph_challenge.
 *
 *  2. IDENTITY LAYOUT — node_ids is the identity (rank r persisted at block r),
 *                    so every persisted block buf[r * 32] is exactly the
 *                    output-set label for rank r.
 *
 *  3. FULL ATTESTATION — super_chunk_blocks == chunk_blocks; every persisted
 *                    block is a challengeable output-set label (attested
 *                    fraction = 1.0, no transient scaffold is persisted).
 *
 *  4. MULTI-CHUNK  — a two-super-chunk buffer uses different chunk seeds for
 *                    the two halves and both halves verify correctly.
 *
 *  5. SCRATCH BUDGET — pose_graph_scratch_bytes_inplace stays bounded
 *                      (proportional to chunk_blocks, not to the region).
 */

#include "../include/graph.h"
#include "../include/hash.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;

#define EXPECT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s: %s\n", __func__, msg); failures++; } \
    else         { fprintf(stdout, "PASS %s: %s\n", __func__, msg); } \
} while (0)

/*
 * Use a small chunk_blocks so the test runs fast on the host (< 1 s).
 * chunk_blocks must be a power of two >= 1.
 * For chunk_blocks=16: super_chunk_blocks ≈ a few thousand, buffer ≈ tens of KB.
 */
#define TEST_CHUNK_BLOCKS 16u

static const uint8_t TEST_SEED[POSE_HASH_BYTES] = {
    0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
    0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
    0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
};

/* Allocate per-thread scratch for the inplace path. */
static int alloc_scratch(uint8_t *scratch[POSE_REGION_THREADS], uint32_t cb)
{
    size_t sz = pose_graph_scratch_bytes_inplace(cb);
    for (int i = 0; i < POSE_REGION_THREADS; i++) {
        scratch[i] = (uint8_t *)malloc(sz);
        if (!scratch[i]) {
            for (int j = 0; j < i; j++) { free(scratch[j]); scratch[j] = NULL; }
            return -1;
        }
    }
    return 0;
}

static void free_scratch(uint8_t *scratch[POSE_REGION_THREADS])
{
    for (int i = 0; i < POSE_REGION_THREADS; i++) { free(scratch[i]); scratch[i] = NULL; }
}

/* ── Test 1 + 2 + 3: single super-chunk ────────────────────────────────────── */

static void test_correctness_and_layout(void)
{
    uint32_t cb           = TEST_CHUNK_BLOCKS;
    uint64_t super_blks   = pose_graph_super_chunk_blocks(cb);
    size_t   super_bytes  = (size_t)super_blks * POSE_HASH_BYTES;

    uint8_t  *buf      = (uint8_t *)calloc(1, super_bytes);
    uint64_t *node_ids = (uint64_t *)malloc((size_t)cb * sizeof(uint64_t));
    uint8_t  *scratch[POSE_REGION_THREADS];

    EXPECT(buf && node_ids, "allocations succeeded");
    if (!buf || !node_ids || alloc_scratch(scratch, cb) != 0) {
        free(buf); free(node_ids);
        fprintf(stderr, "SKIP test_correctness_and_layout (alloc failed)\n");
        return;
    }

    /* Obtain challenge node IDs — seed-independent, uses scratch[0]. */
    pose_graph_challenge_node_ids(cb, node_ids, scratch[0]);

    /* Run inplace labeler on one super-chunk. */
    pose_graph_pool_t *pool = pose_graph_pool_create();
    EXPECT(pool != NULL, "pool created");
    if (!pool) { free(buf); free(node_ids); free_scratch(scratch); return; }

    int rc = pose_graph_label_region_pooled_inplace(
                TEST_SEED, POSE_HASH_BYTES,
                buf, super_bytes, /* region_block_offset= */ 0,
                scratch, pool, POSE_HASH_BLAKE3, cb);
    EXPECT(rc == 0, "inplace labeler returned 0");

    pose_graph_pool_destroy(pool);

    /* Chunk seed for chunk index 0. */
    uint8_t cseed[POSE_HASH_BYTES];
    pose_chunk_seed(cseed, TEST_SEED, POSE_HASH_BYTES, 0);

    /* ── 1. CORRECTNESS ── */
    int all_correct = 1;
    for (uint32_t r = 0; r < cb; r++) {
        uint8_t expected[POSE_HASH_BYTES];
        pose_graph_challenge(cseed, POSE_HASH_BYTES, cb, r, expected, POSE_HASH_BLAKE3);

        uint64_t nid    = node_ids[r];
        const uint8_t *actual = buf + nid * POSE_HASH_BYTES;
        if (memcmp(expected, actual, POSE_HASH_BYTES) != 0) {
            fprintf(stderr, "  rank %u: node_id=%llu  expected[0]=%02x actual[0]=%02x\n",
                    r, (unsigned long long)nid, expected[0], actual[0]);
            all_correct = 0;
        }
    }
    EXPECT(all_correct, "all challenge labels at node_id offsets match pose_graph_challenge");

    /* ── 2. IDENTITY LAYOUT ── */
    /*
     * The faithful prover persists the output set in rank order, so node_ids is
     * the identity and challenge rank r is at the sequential offset buf[r*32].
     */
    int identity_ok = 1, seq_ok = 1;
    for (uint32_t r = 0; r < cb; r++) {
        if (node_ids[r] != (uint64_t)r) identity_ok = 0;
        uint8_t expected[POSE_HASH_BYTES];
        pose_graph_challenge(cseed, POSE_HASH_BYTES, cb, r, expected, POSE_HASH_BLAKE3);
        if (memcmp(expected, buf + (size_t)r * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0)
            seq_ok = 0;
    }
    EXPECT(identity_ok, "challenge node_ids is the identity (rank r at block r)");
    EXPECT(seq_ok, "every sequential offset buf[r*32] holds rank r's output label");

    /* ── 3. FULL ATTESTATION ── */
    /*
     * Every persisted block is an output-set label — there is no transient
     * scaffold in the persisted region, so super_chunk_blocks == chunk_blocks
     * and the attested fraction is 1.0.
     */
    EXPECT(super_blks == (uint64_t)cb,
           "super_chunk_blocks == chunk_blocks (100% of persisted bytes attestable)");

    free(buf);
    free(node_ids);
    free_scratch(scratch);
}

/* ── Test 4: two consecutive super-chunks ───────────────────────────────────── */

static void test_multi_super_chunk(void)
{
    uint32_t cb           = TEST_CHUNK_BLOCKS;
    uint64_t super_blks   = pose_graph_super_chunk_blocks(cb);
    size_t   super_bytes  = (size_t)super_blks * POSE_HASH_BYTES;

    uint8_t  *buf      = (uint8_t *)calloc(1, 2 * super_bytes);
    uint64_t *node_ids = (uint64_t *)malloc((size_t)cb * sizeof(uint64_t));
    uint8_t  *scratch[POSE_REGION_THREADS];

    if (!buf || !node_ids || alloc_scratch(scratch, cb) != 0) {
        free(buf); free(node_ids);
        fprintf(stderr, "SKIP test_multi_super_chunk (alloc failed)\n");
        return;
    }

    pose_graph_challenge_node_ids(cb, node_ids, scratch[0]);

    pose_graph_pool_t *pool = pose_graph_pool_create();
    int rc = pose_graph_label_region_pooled_inplace(
                TEST_SEED, POSE_HASH_BYTES,
                buf, 2 * super_bytes, /* region_block_offset= */ 0,
                scratch, pool, POSE_HASH_BLAKE3, cb);
    EXPECT(rc == 0, "two-super-chunk inplace returned 0");
    pose_graph_pool_destroy(pool);

    uint8_t cseed0[POSE_HASH_BYTES], cseed1[POSE_HASH_BYTES];
    pose_chunk_seed(cseed0, TEST_SEED, POSE_HASH_BYTES, 0);
    pose_chunk_seed(cseed1, TEST_SEED, POSE_HASH_BYTES, 1);

    int c0_ok = 1, c1_ok = 1;
    for (uint32_t r = 0; r < cb; r++) {
        uint8_t exp0[POSE_HASH_BYTES], exp1[POSE_HASH_BYTES];
        pose_graph_challenge(cseed0, POSE_HASH_BYTES, cb, r, exp0, POSE_HASH_BLAKE3);
        pose_graph_challenge(cseed1, POSE_HASH_BYTES, cb, r, exp1, POSE_HASH_BLAKE3);

        uint64_t nid = node_ids[r];
        if (memcmp(exp0, buf + nid * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0) c0_ok = 0;
        if (memcmp(exp1, buf + super_bytes + nid * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0) c1_ok = 0;
    }
    EXPECT(c0_ok, "super-chunk 0: challenge labels correct");
    EXPECT(c1_ok, "super-chunk 1: challenge labels correct");

    /* Two halves must differ — they were derived from different chunk seeds. */
    EXPECT(memcmp(buf, buf + super_bytes, super_bytes) != 0,
           "two super-chunks differ (different chunk seeds)");

    free(buf);
    free(node_ids);
    free_scratch(scratch);
}

/* ── Test 5: scratch budget ─────────────────────────────────────────────────── */

static void test_scratch_budget(void)
{
    /*
     * The faithful prover keeps the full scaffold transient in a bounded label
     * arena (proportional to chunk_blocks, NOT to the region) and persists only
     * the output set.  Two properties:
     *
     *   - the persisted super-chunk shrinks from scratch_node_count(cb)·32 to
     *     cb·32, so every persisted byte is a challengeable output label, and
     *   - the transient scratch stays bounded by (4 + POSE_ARENA_FACTOR)·half_w
     *     label slots — far below the old full-scaffold label buffer.
     */
    uint32_t cb = POSE_CHUNK_BLOCKS;
    size_t inplace_sz   = pose_graph_scratch_bytes_inplace(cb);
    size_t old_label_sz = pose_graph_scratch_bytes(cb); /* old full-scaffold path */
    uint64_t new_super  = pose_graph_super_chunk_blocks(cb);

    printf("  super-chunk (faithful): %llu blocks (%llu KB persisted, 100%% attested)\n",
           (unsigned long long)new_super,
           (unsigned long long)(new_super * POSE_HASH_BYTES >> 10));
    printf("  scratch (inplace)    : %zu KB / thread\n", inplace_sz / 1024);
    printf("  old full-scaffold scratch: %zu KB / thread\n", old_label_sz / 1024);

    EXPECT(new_super == (uint64_t)cb,
           "faithful super-chunk persists exactly chunk_blocks output labels");
    EXPECT(inplace_sz < old_label_sz,
           "transient scratch is smaller than the old full-scaffold label buffer");
}

/* ── Test: non-zero region_block_offset ─────────────────────────────────────── */

static void test_block_offset(void)
{
    /*
     * Label the same buffer at two different region_block_offsets.
     * The labels must differ because pose_chunk_seed depends on the
     * chunk_index derived from (region_block_offset / super_blocks).
     */
    uint32_t cb          = TEST_CHUNK_BLOCKS;
    uint64_t super_blks  = pose_graph_super_chunk_blocks(cb);
    size_t   super_bytes = (size_t)super_blks * POSE_HASH_BYTES;

    uint8_t *buf0 = (uint8_t *)malloc(super_bytes);
    uint8_t *buf1 = (uint8_t *)malloc(super_bytes);
    uint8_t *scratch[POSE_REGION_THREADS];

    if (!buf0 || !buf1 || alloc_scratch(scratch, cb) != 0) {
        free(buf0); free(buf1);
        fprintf(stderr, "SKIP test_block_offset (alloc failed)\n");
        return;
    }

    pose_graph_pool_t *pool = pose_graph_pool_create();

    /* region_block_offset=0 → chunk_index = (0 + 0*cb)/cb = 0 */
    pose_graph_label_region_pooled_inplace(
        TEST_SEED, POSE_HASH_BYTES, buf0, super_bytes, 0,
        scratch, pool, POSE_HASH_BLAKE3, cb);

    /*
     * region_block_offset=cb → chunk_index = (cb + 0*cb)/cb = 1.
     *
     * The inplace formula is (region_block_offset + j*cb) / cb, mirroring
     * pose_graph_label_region.  Using cb (not super_blks) as the offset
     * produces chunk_index=1 cleanly regardless of super_blks.
     */
    pose_graph_label_region_pooled_inplace(
        TEST_SEED, POSE_HASH_BYTES, buf1, super_bytes, (uint64_t)cb,
        scratch, pool, POSE_HASH_BLAKE3, cb);

    pose_graph_pool_destroy(pool);

    EXPECT(memcmp(buf0, buf1, super_bytes) != 0,
           "different region_block_offset → different labels");

    /*
     * Verify buf1 against cseed for chunk_index=1 to confirm the offset
     * mapping is correct, not just "different".
     */
    uint64_t *node_ids = (uint64_t *)malloc((size_t)cb * sizeof(uint64_t));
    pose_graph_challenge_node_ids(cb, node_ids, scratch[0]);

    uint8_t cseed1[POSE_HASH_BYTES];
    pose_chunk_seed(cseed1, TEST_SEED, POSE_HASH_BYTES, 1);

    int ok = 1;
    for (uint32_t r = 0; r < cb; r++) {
        uint8_t expected[POSE_HASH_BYTES];
        pose_graph_challenge(cseed1, POSE_HASH_BYTES, cb, r, expected, POSE_HASH_BLAKE3);
        if (memcmp(expected, buf1 + node_ids[r] * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0) {
            ok = 0; break;
        }
    }
    EXPECT(ok, "buf at offset super_blks verifies against chunk_index=1 seed");

    free(buf0); free(buf1); free(node_ids);
    free_scratch(scratch);
}

int main(void)
{
    printf("super_chunk_blocks(TEST_CHUNK_BLOCKS=%u) = %llu\n",
           TEST_CHUNK_BLOCKS,
           (unsigned long long)pose_graph_super_chunk_blocks(TEST_CHUNK_BLOCKS));

    test_correctness_and_layout();
    test_multi_super_chunk();
    test_block_offset();
    test_scratch_budget();

    if (failures) {
        fprintf(stderr, "\n%d test(s) FAILED\n", failures);
        return 1;
    }
    fprintf(stdout, "\nAll inplace tests passed\n");
    return 0;
}
