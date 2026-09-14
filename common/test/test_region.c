/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * Tests for pose_graph_label_region.
 *
 * Verifies that each block written by the labeler matches the label that
 * pose_graph_challenge would return for the same seed and block index.
 * This is the critical protocol agreement test between daemon and verifier.
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
 * Verify every block in `region_data` against pose_graph_challenge.
 * Mirrors the chunk loop in pose_graph_label_region exactly.
 */
static int verify_region(const uint8_t *region_data, uint64_t total_blocks,
                          const uint8_t *session_seed, size_t seed_len,
                          uint64_t region_block_offset)
{
    uint64_t block = 0;
    while (block < total_blocks) {
        uint64_t this_blocks = total_blocks - block;
        if (this_blocks > POSE_CHUNK_BLOCKS)
            this_blocks = POSE_CHUNK_BLOCKS;

        uint64_t chunk_index = (region_block_offset + block) / POSE_CHUNK_BLOCKS;

        uint8_t cseed[POSE_HASH_BYTES];
        pose_chunk_seed(cseed, session_seed, seed_len, chunk_index);

        for (uint64_t i = 0; i < this_blocks; i++) {
            uint8_t expected[POSE_HASH_BYTES];
            if (pose_graph_challenge(cseed, POSE_HASH_BYTES, this_blocks, i, expected, POSE_HASH_BLAKE3) != 0)
                return -1;
            if (memcmp(expected,
                       region_data + (block + i) * POSE_HASH_BYTES,
                       POSE_HASH_BYTES) != 0)
                return -1;
        }
        block += this_blocks;
    }
    return 0;
}

static void test_region_zero_offset(void)
{
    uint8_t seed[POSE_HASH_BYTES] = {
        0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04,
        0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c,
        0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14,
        0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c,
    };
    const uint64_t n_blocks = 16;
    uint8_t *buf = (uint8_t *)malloc(n_blocks * POSE_HASH_BYTES);

    int rc = pose_graph_label_region(seed, POSE_HASH_BYTES, buf, n_blocks * POSE_HASH_BYTES, 0, POSE_HASH_BLAKE3, POSE_CHUNK_BLOCKS);
    EXPECT(rc == 0, "label_region returns 0");

    int ok = verify_region(buf, n_blocks, seed, POSE_HASH_BYTES, 0) == 0;
    EXPECT(ok, "all blocks match challenge at offset 0");

    free(buf);
}

static void test_region_nonzero_offset(void)
{
    /* Region starting at block offset POSE_CHUNK_BLOCKS — uses chunk seed 1, not 0. */
    uint8_t seed[POSE_HASH_BYTES] = {
        0xca, 0xfe, 0xba, 0xbe, 0xde, 0xad, 0xc0, 0xde,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
        0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    const uint64_t n_blocks = 16;
    const uint64_t block_offset = POSE_CHUNK_BLOCKS; /* chunk 1 */
    uint8_t *buf = (uint8_t *)malloc(n_blocks * POSE_HASH_BYTES);

    int rc = pose_graph_label_region(seed, POSE_HASH_BYTES, buf,
                                      n_blocks * POSE_HASH_BYTES, block_offset, POSE_HASH_BLAKE3,
                                      POSE_CHUNK_BLOCKS);
    EXPECT(rc == 0, "label_region returns 0 with nonzero offset");

    int ok = verify_region(buf, n_blocks, seed, POSE_HASH_BYTES, block_offset) == 0;
    EXPECT(ok, "all blocks match challenge at chunk-aligned offset");

    /* Sanity: labels differ from same-m labeling at offset 0 (different chunk seed). */
    uint8_t *buf0 = (uint8_t *)malloc(n_blocks * POSE_HASH_BYTES);
    pose_graph_label_region(seed, POSE_HASH_BYTES, buf0, n_blocks * POSE_HASH_BYTES, 0, POSE_HASH_BLAKE3, POSE_CHUNK_BLOCKS);
    int different = memcmp(buf, buf0, n_blocks * POSE_HASH_BYTES) != 0;
    EXPECT(different, "chunk 1 labels differ from chunk 0 labels");

    free(buf);
    free(buf0);
}

static void test_region_determinism(void)
{
    uint8_t seed[POSE_HASH_BYTES] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    };
    const uint64_t n_blocks = 8;
    uint8_t *buf1 = (uint8_t *)malloc(n_blocks * POSE_HASH_BYTES);
    uint8_t *buf2 = (uint8_t *)malloc(n_blocks * POSE_HASH_BYTES);

    pose_graph_label_region(seed, POSE_HASH_BYTES, buf1, n_blocks * POSE_HASH_BYTES, 0, POSE_HASH_BLAKE3, POSE_CHUNK_BLOCKS);
    pose_graph_label_region(seed, POSE_HASH_BYTES, buf2, n_blocks * POSE_HASH_BYTES, 0, POSE_HASH_BLAKE3, POSE_CHUNK_BLOCKS);

    EXPECT(memcmp(buf1, buf2, n_blocks * POSE_HASH_BYTES) == 0,
           "same seed + offset → same labels");

    free(buf1);
    free(buf2);
}

static void test_region_seed_sensitivity(void)
{
    uint8_t seed_a[POSE_HASH_BYTES] = { 0xaa };  /* rest zero */
    uint8_t seed_b[POSE_HASH_BYTES] = { 0xbb };  /* rest zero */
    const uint64_t n_blocks = 8;
    uint8_t *buf_a = (uint8_t *)malloc(n_blocks * POSE_HASH_BYTES);
    uint8_t *buf_b = (uint8_t *)malloc(n_blocks * POSE_HASH_BYTES);

    pose_graph_label_region(seed_a, POSE_HASH_BYTES, buf_a, n_blocks * POSE_HASH_BYTES, 0, POSE_HASH_BLAKE3, POSE_CHUNK_BLOCKS);
    pose_graph_label_region(seed_b, POSE_HASH_BYTES, buf_b, n_blocks * POSE_HASH_BYTES, 0, POSE_HASH_BLAKE3, POSE_CHUNK_BLOCKS);

    EXPECT(memcmp(buf_a, buf_b, n_blocks * POSE_HASH_BYTES) != 0,
           "different seeds → different labels");

    free(buf_a);
    free(buf_b);
}

int main(void)
{
    test_region_zero_offset();
    test_region_nonzero_offset();
    test_region_determinism();
    test_region_seed_sensitivity();

    if (failures) {
        fprintf(stderr, "\n%d test(s) FAILED\n", failures);
        return 1;
    }
    fprintf(stdout, "\nAll region tests passed\n");
    return 0;
}
