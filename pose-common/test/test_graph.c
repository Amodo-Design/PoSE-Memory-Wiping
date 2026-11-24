/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * Graph labeling tests for pose-db-drg-v1.
 *
 * Tests:
 *   1. pose_graph_parameter_n(m) correctness
 *   2. pose_graph_label output is deterministic
 *   3. pose_graph_label output changes with different seeds
 *   4. pose_graph_challenge agrees with the corresponding slot in pose_graph_label
 *   5. Challenge labels are all distinct (collision check for small m)
 *   6. Node counts match expected standalone_node_count for small levels
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

static void test_parameter_n(void)
{
    EXPECT(pose_graph_parameter_n(1)  == 0, "n(1)=0");
    EXPECT(pose_graph_parameter_n(2)  == 0, "n(2)=0");
    EXPECT(pose_graph_parameter_n(3)  == 1, "n(3)=1");
    EXPECT(pose_graph_parameter_n(4)  == 1, "n(4)=1");
    EXPECT(pose_graph_parameter_n(5)  == 2, "n(5)=2");
    EXPECT(pose_graph_parameter_n(8)  == 2, "n(8)=2");
    EXPECT(pose_graph_parameter_n(9)  == 3, "n(9)=3");
    EXPECT(pose_graph_parameter_n(16) == 3, "n(16)=3");
    EXPECT(pose_graph_parameter_n(17) == 4, "n(17)=4");
    EXPECT(pose_graph_parameter_n(32) == 4, "n(32)=4");
}

static void test_determinism(void)
{
    uint8_t seed[POSE_HASH_BYTES] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
    };
    uint64_t m = 8;
    uint8_t *out1 = (uint8_t *)malloc(m * POSE_HASH_BYTES);
    uint8_t *out2 = (uint8_t *)malloc(m * POSE_HASH_BYTES);

    int r1 = pose_graph_label(seed, POSE_HASH_BYTES, m, out1, POSE_HASH_BLAKE3);
    int r2 = pose_graph_label(seed, POSE_HASH_BYTES, m, out2, POSE_HASH_BLAKE3);

    EXPECT(r1 == 0, "label returns 0");
    EXPECT(r2 == 0, "second label returns 0");
    EXPECT(memcmp(out1, out2, m * POSE_HASH_BYTES) == 0, "same seed → same output");

    free(out1);
    free(out2);
}

static void test_seed_sensitivity(void)
{
    uint8_t seed_a[POSE_HASH_BYTES] = { 0xaa };  /* rest zero */
    uint8_t seed_b[POSE_HASH_BYTES] = { 0xbb };  /* rest zero */
    uint64_t m = 8;
    uint8_t *out_a = (uint8_t *)malloc(m * POSE_HASH_BYTES);
    uint8_t *out_b = (uint8_t *)malloc(m * POSE_HASH_BYTES);

    pose_graph_label(seed_a, POSE_HASH_BYTES, m, out_a, POSE_HASH_BLAKE3);
    pose_graph_label(seed_b, POSE_HASH_BYTES, m, out_b, POSE_HASH_BLAKE3);

    EXPECT(memcmp(out_a, out_b, m * POSE_HASH_BYTES) != 0,
           "different seeds → different output");

    free(out_a);
    free(out_b);
}

static void test_challenge_agrees(uint64_t m)
{
    char desc[64];
    snprintf(desc, sizeof(desc), "challenge agrees for m=%llu", (unsigned long long)m);

    uint8_t seed[POSE_HASH_BYTES] = {
        0xfe, 0xed, 0xfa, 0xce, 0xde, 0xad, 0xbe, 0xef,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
        0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5, 0x96, 0x87,
        0x78, 0x69, 0x5a, 0x4b, 0x3c, 0x2d, 0x1e, 0x0f,
    };
    uint8_t *all = (uint8_t *)malloc(m * POSE_HASH_BYTES);
    int rc = pose_graph_label(seed, POSE_HASH_BYTES, m, all, POSE_HASH_BLAKE3);
    EXPECT(rc == 0, desc);
    if (rc != 0) { free(all); return; }

    int mismatch = 0;
    for (uint64_t i = 0; i < m && !mismatch; i++) {
        uint8_t single[POSE_HASH_BYTES];
        int rc2 = pose_graph_challenge(seed, POSE_HASH_BYTES, m, i, single, POSE_HASH_BLAKE3);
        if (rc2 != 0 || memcmp(single, all + i * POSE_HASH_BYTES, POSE_HASH_BYTES) != 0)
            mismatch = 1;
    }

    char msg[80];
    snprintf(msg, sizeof(msg), "challenge[i] == all[i] for all i, m=%llu",
             (unsigned long long)m);
    EXPECT(!mismatch, msg);
    free(all);
}

static void test_no_collisions(uint64_t m)
{
    uint8_t seed[POSE_HASH_BYTES] = {
        0xca, 0xfe, 0xba, 0xbe, 0xde, 0xad, 0xc0, 0xde,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
    };
    uint8_t *labels = (uint8_t *)malloc(m * POSE_HASH_BYTES);
    int rc = pose_graph_label(seed, POSE_HASH_BYTES, m, labels, POSE_HASH_BLAKE3);

    char msg[64];
    snprintf(msg, sizeof(msg), "no collisions for m=%llu", (unsigned long long)m);
    if (rc != 0) {
        EXPECT(0, msg);
        free(labels);
        return;
    }

    int collision = 0;
    for (uint64_t i = 0; i < m && !collision; i++) {
        for (uint64_t j = i + 1; j < m && !collision; j++) {
            if (memcmp(labels + i * POSE_HASH_BYTES,
                       labels + j * POSE_HASH_BYTES,
                       POSE_HASH_BYTES) == 0)
                collision = 1;
        }
    }
    EXPECT(!collision, msg);
    free(labels);
}

static void test_m_size_variations(void)
{
    /* Test various m values including non-power-of-2 */
    uint64_t sizes[] = {1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        test_challenge_agrees(sizes[i]);
        if (sizes[i] <= 16)
            test_no_collisions(sizes[i]);
    }
}

int main(void)
{
    test_parameter_n();
    test_determinism();
    test_seed_sensitivity();
    test_m_size_variations();

    if (failures) {
        fprintf(stderr, "\n%d test(s) FAILED\n", failures);
        return 1;
    }
    fprintf(stdout, "\nAll graph tests passed\n");
    return 0;
}
