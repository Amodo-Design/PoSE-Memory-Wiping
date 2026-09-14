/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_AES_PRF_H
#define POSE_AES_PRF_H

#include <stddef.h>
#include <stdint.h>

/*
 * AES-128-CBC-MAC double-pipe label PRF for PoSE-DB.
 *
 * Compact 112-byte message (7 × 16-byte AES blocks):
 *   Bytes  0–7:   node_index big-endian
 *   Bytes  8–15:  zero padding
 *   Bytes 16–47:  pred0 label (32 B; zero if source node or absent)
 *   Bytes 48–79:  pred1 label (32 B; zero if source node or absent)
 *   Bytes 80–111: graph descriptor (32 B)
 *
 * Key from chunk_seed (32 bytes):
 *   key_hi = seed[0:16],  key_lo = seed[16:32]
 *   label  = CBC-MAC(key_hi, msg) || CBC-MAC(key_lo, msg)   → 32 bytes
 *
 * Requires __ARM_FEATURE_AES (compile src/aes_prf.c with -march=armv8-a+crypto).
 * Aborts if called on a platform without AES-CE support.
 */

/*
 * Hash n (1 ≤ n ≤ 4) independent labels.  All n share the same seed and
 * descriptor.  For n == 4 the implementation uses an 8-chain interleaved
 * AES-CE path; for n < 4 it falls back to per-label scalar chains.
 *
 *   outs          n × 32 output bytes (label i at outs + i*32)
 *   is_source     1 = source node; pred0s/pred1s are ignored (zeroed)
 *   node_indices  n node IDs
 *   pred0s/pred1s n predecessor label pointers (NULL → treated as 32 zero bytes)
 *   seed          32-byte chunk seed (split into key_hi / key_lo)
 *   descriptor    32-byte graph descriptor
 */
void pose_aes_prf_many(size_t n,
                        uint8_t *outs,
                        int is_source,
                        const uint64_t *node_indices,
                        const uint8_t *const *pred0s,
                        const uint8_t *const *pred1s,
                        const uint8_t *seed,
                        const uint8_t *descriptor);

#endif /* POSE_AES_PRF_H */
