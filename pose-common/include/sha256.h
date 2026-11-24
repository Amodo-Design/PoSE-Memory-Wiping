/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_SHA256_H
#define POSE_SHA256_H

#include <stdint.h>
#include <stddef.h>

/*
 * SHA-256 of an arbitrary message.
 *
 * On AArch64 with __ARM_FEATURE_SHA2: uses sha256h / sha256h2 / sha256su0 /
 * sha256su1 hardware instructions (compiled with -march=armv8-a+sha2).
 * On all other targets: portable C implementation.
 *
 * Used by the POSE_HASH_SHA256 path in pose_label_node / pose_label_node_many
 * to benchmark ARMv8 SHA-2 hardware acceleration against BLAKE3 NEON batching.
 * No domain separation or keying — throughput comparison only.
 */
void pose_sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32]);

#endif /* POSE_SHA256_H */
