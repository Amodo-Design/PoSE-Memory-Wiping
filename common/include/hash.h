/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_HASH_H
#define POSE_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define POSE_HASH_BYTES 32

/*
 * Hash algorithm selector — passed through every labeling call.
 *
 * POSE_HASH_BLAKE3:   blake3_hash_many NEON 4-wide (ARM64).  192-byte message.
 *                     Domain-separated keying via derive_key contexts.
 *                     Production default.
 * POSE_HASH_SHA256:   ARMv8 SHA-2 hardware (one message at a time).
 *                     192-byte message.  No domain-separated keying —
 *                     throughput comparison only, not for production use.
 * POSE_HASH_AES_PRF:  AES-128-CBC-MAC double-pipe (ARM AES-CE).
 *                     112-byte compact message; chunk_seed is the AES key
 *                     rather than appearing in the message body.
 *                     Requires -march=armv8-a+crypto (ARMv8 AES extension) or
 *                     -maes (x86 AES-NI).
 */
typedef enum {
    POSE_HASH_BLAKE3  = 0,
    POSE_HASH_SHA256  = 1,
    POSE_HASH_AES_PRF = 2,
} pose_hash_algo_t;

static inline const char *pose_hash_to_string(pose_hash_algo_t algo) {
    if (algo == POSE_HASH_BLAKE3) {
        return "blake3";
    } else if (algo == POSE_HASH_SHA256) {
        return "sha256";
    } else {
        return "aesprf"; 
    }
}

/*
 * Uniform 192-byte node message layout (3 × 64-byte blocks):
 *
 *   Block 0 (64 B): [ node_index:8 big-endian | zeros:56 ]
 *   Block 1 (64 B): [ pred0:32 | pred1:32 ]  (zero-filled when pred absent)
 *   Block 2 (64 B): [ seed:32 | descriptor:32 ]
 *
 * BLAKE3 source nodes key: "pose-db/label/src"
 * BLAKE3 internal nodes key: "pose-db/label/int"
 * seed must be exactly POSE_HASH_BYTES (32) bytes.
 */

/*
 * Derive the label for a single node.
 *
 *   is_source  1 = source node (pred0/pred1 ignored; treated as zero blocks)
 *              0 = internal node
 *   pred0/pred1  NULL → treated as 32 zero bytes
 *   seed must be exactly POSE_HASH_BYTES bytes
 *   algo  selects the hash function for this call
 */
void pose_label_node(uint8_t out[POSE_HASH_BYTES],
                     int is_source,
                     uint64_t node_index,
                     const uint8_t *pred0,
                     const uint8_t *pred1,
                     const uint8_t *seed,
                     const uint8_t descriptor[POSE_HASH_BYTES],
                     pose_hash_algo_t algo);

/*
 * Maximum nodes per pose_label_node_many() call.  16 = the widest BLAKE3
 * hash_many SIMD backend (AVX-512); blake3_hash_many internally dispatches to
 * the widest lane width the running CPU supports (AVX-512 → AVX2 → SSE4.1 →
 * portable), so passing a full batch of 16 lets it use the machine's maximum
 * without the caller knowing the CPU.  Batch width never changes any output
 * label byte (intra-layer nodes are independent; lanes are pure parallelism),
 * so this is parity-preserving against the GPU and the verifier.
 */
#define POSE_LABEL_BATCH 16

/*
 * Batch label derivation for n nodes (1 ≤ n ≤ POSE_LABEL_BATCH).
 *
 * All nodes must share the same is_source value and the same seed/descriptor.
 * pred0s[i] / pred1s[i] may be NULL (treated as zero block).
 * Passing NULL for pred0s or pred1s treats all n entries as NULL.
 * outs must be n * POSE_HASH_BYTES bytes; output i starts at outs + i*32.
 *
 * BLAKE3 path: routes through blake3_hash_many — uses the widest SIMD backend
 *   the CPU supports (up to AVX-512 16-wide on x86, NEON 4-wide on ARM64).
 * SHA-256 path: n sequential single-message SHA-256 calls.
 * AES-PRF path: processed in 4-wide groups (its hardware kernel is 4-wide).
 */
void pose_label_node_many(size_t n,
                          int is_source,
                          const uint64_t *node_indices,
                          const uint8_t *const *pred0s,
                          const uint8_t *const *pred1s,
                          const uint8_t *seed,
                          const uint8_t descriptor[POSE_HASH_BYTES],
                          uint8_t *outs,
                          pose_hash_algo_t algo);

/*
 * Copy the two BLAKE3 derived label keys (each 8 little-endian uint32 words,
 * the format blake3_hash_many / a raw compression accepts directly):
 *   key_src ← derive_key("pose-db/label/src")  — source nodes
 *   key_int ← derive_key("pose-db/label/int")  — internal nodes
 *
 * Exposed so the CUDA HBM labeler can feed identical keys to its on-device
 * keyed BLAKE3, guaranteeing byte-identical labels to the CPU path.  Only
 * meaningful for POSE_HASH_BLAKE3.
 */
void pose_label_node_keys(uint32_t key_src[8], uint32_t key_int[8]);

/*
 * Graph descriptor digest — domain-separates labels per (m, n, label_width_bits).
 *
 * Context "pose-db/descriptor".
 * Input: uint64_be(m) || uint64_be(n) || uint64_be(256)
 */
void pose_graph_descriptor(uint8_t out[POSE_HASH_BYTES], uint64_t m, uint64_t n);

/*
 * Derive a per-chunk seed from the session seed and a chunk index.
 * Used by pose_graph_label_region to give each chunk an independent key.
 *
 * Context "pose-db/chunk-seed".
 * Input: session_seed || uint64_be(chunk_index)
 */
void pose_chunk_seed(uint8_t out[POSE_HASH_BYTES],
                     const uint8_t *session_seed, size_t seed_len,
                     uint64_t chunk_index);

#ifdef __cplusplus
}
#endif

#endif /* POSE_HASH_H */
