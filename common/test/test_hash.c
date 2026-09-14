/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * BLAKE3 test vectors (from https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json)
 * plus property tests for the pose label derivation functions.
 */
#include "../include/hash.h"
#include "../vendor/blake3/blake3.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;

static void hex(const uint8_t *d, int n, char *out)
{
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i*2]   = h[d[i] >> 4];
        out[i*2+1] = h[d[i] & 0xf];
    }
    out[n*2] = '\0';
}

static void check_blake3(const char *desc, const void *data, size_t len,
                          const char *expected_hex)
{
    uint8_t digest[POSE_HASH_BYTES];
    char got[POSE_HASH_BYTES * 2 + 1];

    blake3_hasher h;
    blake3_hasher_init(&h);
    blake3_hasher_update(&h, data, len);
    blake3_hasher_finalize(&h, digest, POSE_HASH_BYTES);
    hex(digest, POSE_HASH_BYTES, got);

    if (strcmp(got, expected_hex) != 0) {
        fprintf(stderr, "FAIL %s\n  got: %s\n  exp: %s\n", desc, got, expected_hex);
        failures++;
    } else {
        fprintf(stdout, "PASS %s\n", desc);
    }
}

static void test_blake3_vectors(void)
{
    /* Official BLAKE3 test vectors — first 32 bytes of output.
     * Input is a byte sequence 0, 1, 2, ... of the given length.
     * Vectors from https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json */

    /* empty input */
    check_blake3("blake3_empty", "", 0,
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");

    /* 1-byte input: 0x00 */
    static const uint8_t one_byte[1] = {0x00};
    check_blake3("blake3_1byte", one_byte, 1,
        "2d3adedff11b61f14c886e35afa036736dcd87a74d27b5c1510225d0f592e213");

    /* 1024-byte input: i % 251 (official BLAKE3 test vector pattern) */
    uint8_t buf1024[1024];
    for (int i = 0; i < 1024; i++) buf1024[i] = (uint8_t)(i % 251);
    check_blake3("blake3_1024bytes", buf1024, 1024,
        "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7");
}

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* 32-byte test seed */
static const uint8_t TEST_SEED[POSE_HASH_BYTES] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
    0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
};

/* ── label property tests ─────────────────────────────────────────────────── */

static void test_source_label_determinism(void)
{
    uint8_t desc[POSE_HASH_BYTES];
    pose_graph_descriptor(desc, 16, 3);

    uint8_t out1[POSE_HASH_BYTES], out2[POSE_HASH_BYTES];
    pose_label_node(out1, 1, 42, NULL, NULL, TEST_SEED, desc, POSE_HASH_BLAKE3);
    pose_label_node(out2, 1, 42, NULL, NULL, TEST_SEED, desc, POSE_HASH_BLAKE3);

    if (memcmp(out1, out2, POSE_HASH_BYTES) != 0) {
        fprintf(stderr, "FAIL source_label_determinism\n");
        failures++;
    } else {
        fprintf(stdout, "PASS source_label_determinism\n");
    }
}

static void test_source_label_uniqueness(void)
{
    uint8_t desc[POSE_HASH_BYTES];
    pose_graph_descriptor(desc, 16, 3);

    uint8_t out0[POSE_HASH_BYTES], out1[POSE_HASH_BYTES];
    pose_label_node(out0, 1, 0, NULL, NULL, TEST_SEED, desc, POSE_HASH_BLAKE3);
    pose_label_node(out1, 1, 1, NULL, NULL, TEST_SEED, desc, POSE_HASH_BLAKE3);

    if (memcmp(out0, out1, POSE_HASH_BYTES) == 0) {
        fprintf(stderr, "FAIL source_label_uniqueness: node 0 == node 1\n");
        failures++;
    } else {
        fprintf(stdout, "PASS source_label_uniqueness\n");
    }
}

static void test_internal_label_vs_source(void)
{
    uint8_t desc[POSE_HASH_BYTES];
    pose_graph_descriptor(desc, 16, 3);

    uint8_t pred[POSE_HASH_BYTES];
    memset(pred, 0xaa, POSE_HASH_BYTES);

    uint8_t src[POSE_HASH_BYTES], int1[POSE_HASH_BYTES];
    pose_label_node(src,  1, 7, NULL, NULL, TEST_SEED, desc, POSE_HASH_BLAKE3);
    pose_label_node(int1, 0, 7, pred,  NULL, TEST_SEED, desc, POSE_HASH_BLAKE3);

    if (memcmp(src, int1, POSE_HASH_BYTES) == 0) {
        fprintf(stderr, "FAIL source_vs_internal: same output for different arity\n");
        failures++;
    } else {
        fprintf(stdout, "PASS source_vs_internal\n");
    }
}

static void test_descriptor_uniqueness(void)
{
    uint8_t d1[POSE_HASH_BYTES], d2[POSE_HASH_BYTES];
    pose_graph_descriptor(d1, 16, 3);
    pose_graph_descriptor(d2, 32, 4);

    if (memcmp(d1, d2, POSE_HASH_BYTES) == 0) {
        fprintf(stderr, "FAIL descriptor_uniqueness: m=16,n=3 == m=32,n=4\n");
        failures++;
    } else {
        fprintf(stdout, "PASS descriptor_uniqueness\n");
    }
}

static void test_chunk_seed_determinism(void)
{
    uint8_t out1[POSE_HASH_BYTES], out2[POSE_HASH_BYTES];
    pose_chunk_seed(out1, TEST_SEED, POSE_HASH_BYTES, 0);
    pose_chunk_seed(out2, TEST_SEED, POSE_HASH_BYTES, 0);
    if (memcmp(out1, out2, POSE_HASH_BYTES) != 0) {
        fprintf(stderr, "FAIL chunk_seed_determinism\n"); failures++;
    } else {
        fprintf(stdout, "PASS chunk_seed_determinism\n");
    }
}

static void test_chunk_seed_uniqueness(void)
{
    uint8_t out0[POSE_HASH_BYTES], out1[POSE_HASH_BYTES];
    pose_chunk_seed(out0, TEST_SEED, POSE_HASH_BYTES, 0);
    pose_chunk_seed(out1, TEST_SEED, POSE_HASH_BYTES, 1);
    if (memcmp(out0, out1, POSE_HASH_BYTES) == 0) {
        fprintf(stderr, "FAIL chunk_seed_uniqueness: chunk 0 == chunk 1\n"); failures++;
    } else {
        fprintf(stdout, "PASS chunk_seed_uniqueness\n");
    }
}

static void test_chunk_seed_domain_separation(void)
{
    uint8_t seed[POSE_HASH_BYTES];
    memset(seed, 0x55, sizeof(seed));
    uint8_t out[POSE_HASH_BYTES];
    pose_chunk_seed(out, seed, POSE_HASH_BYTES, 0);
    if (memcmp(out, seed, POSE_HASH_BYTES) == 0) {
        fprintf(stderr, "FAIL chunk_seed_domain_separation: output == input\n"); failures++;
    } else {
        fprintf(stdout, "PASS chunk_seed_domain_separation\n");
    }
}

/*
 * pose_label_node_many(n) must produce the same output as n sequential
 * pose_label_node calls with the same inputs.
 */
static void test_batch_consistency(void)
{
    uint8_t desc[POSE_HASH_BYTES];
    pose_graph_descriptor(desc, 64, 5);

    /* Build 4 different predecessor buffers */
    uint8_t pred0_bufs[4][POSE_HASH_BYTES];
    uint8_t pred1_bufs[4][POSE_HASH_BYTES];
    for (int i = 0; i < 4; i++) {
        memset(pred0_bufs[i], (uint8_t)(0x10 + i), POSE_HASH_BYTES);
        memset(pred1_bufs[i], (uint8_t)(0x20 + i), POSE_HASH_BYTES);
    }

    uint64_t node_indices[4] = { 100, 101, 102, 103 };
    const uint8_t *pred0s[4] = {
        pred0_bufs[0], pred0_bufs[1], pred0_bufs[2], pred0_bufs[3]
    };
    const uint8_t *pred1s[4] = {
        pred1_bufs[0], pred1_bufs[1], pred1_bufs[2], pred1_bufs[3]
    };

    /* Sequential reference */
    uint8_t seq_out[4][POSE_HASH_BYTES];
    for (int i = 0; i < 4; i++) {
        pose_label_node(seq_out[i], 0, node_indices[i],
                        pred0s[i], pred1s[i], TEST_SEED, desc, POSE_HASH_BLAKE3);
    }

    /* Batch */
    uint8_t batch_out[4 * POSE_HASH_BYTES];
    pose_label_node_many(4, 0, node_indices, pred0s, pred1s,
                         TEST_SEED, desc, batch_out, POSE_HASH_BLAKE3);

    int ok = 1;
    for (int i = 0; i < 4; i++) {
        if (memcmp(seq_out[i], batch_out + i * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0) {
            ok = 0;
            fprintf(stderr, "FAIL batch_consistency: mismatch at index %d\n", i);
            failures++;
        }
    }
    if (ok)
        fprintf(stdout, "PASS batch_consistency\n");

    /* Also test source batch */
    uint8_t seq_src[4][POSE_HASH_BYTES];
    for (int i = 0; i < 4; i++)
        pose_label_node(seq_src[i], 1, node_indices[i], NULL, NULL, TEST_SEED, desc, POSE_HASH_BLAKE3);

    uint8_t batch_src[4 * POSE_HASH_BYTES];
    pose_label_node_many(4, 1, node_indices, NULL, NULL, TEST_SEED, desc, batch_src, POSE_HASH_BLAKE3);

    ok = 1;
    for (int i = 0; i < 4; i++) {
        if (memcmp(seq_src[i], batch_src + i * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0) {
            ok = 0;
            fprintf(stderr, "FAIL batch_consistency_source: mismatch at index %d\n", i);
            failures++;
        }
    }
    if (ok)
        fprintf(stdout, "PASS batch_consistency_source\n");
}

int main(void)
{
    test_blake3_vectors();
    test_source_label_determinism();
    test_source_label_uniqueness();
    test_internal_label_vs_source();
    test_descriptor_uniqueness();
    test_chunk_seed_determinism();
    test_chunk_seed_uniqueness();
    test_chunk_seed_domain_separation();
    test_batch_consistency();

    if (failures) {
        fprintf(stderr, "\n%d test(s) FAILED\n", failures);
        return 1;
    }
    fprintf(stdout, "\nAll hash tests passed\n");
    return 0;
}
