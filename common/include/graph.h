/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_GRAPH_H
#define POSE_GRAPH_H

#include "hash.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PoSE-DB graph labeling — pose-db-drg-v1
 *
 * The graph G for label_count_m blocks is two standalone copies of the
 * recursive depth-robust DAG G_{n+1}, where n is the smallest integer such
 * that 2^(n+1) >= m.  The challenge set O(G) has exactly m nodes.
 *
 * This file exposes two operations:
 *
 *  1. pose_graph_label        — prover: label the full target region
 *  2. pose_graph_challenge    — verifier: recompute one challenge label
 *
 * Both use the formula-driven topological traversal so no explicit edge table
 * is stored.  The implementation is correct for any m.  The full-scratch path
 * allocates a complete label buffer (2·S(n+1) labels) for simplicity; the
 * in-place path below bounds the working set to O(m·w).
 *
 * Maximum supported m for the full-scratch path: 2^20 = 1,048,576 blocks.
 * Beyond that, the scratch buffer exceeds 1 GB and a streaming approach is
 * required (the in-place path below).
 */

#define POSE_GRAPH_MAX_M (1u << 20)

/*
 * Default blocks per chunk for pose_graph_label_region.
 *
 * Each chunk is labeled independently with a derived seed.
 * Chunk size = POSE_CHUNK_BLOCKS * POSE_HASH_BYTES = 4096 * 32 = 128 KB.
 * Peak scratch per chunk ≈ 2 * S(13) * 32 ≈ 42 MB — constant regardless
 * of total region size.
 *
 * This default is used when chunk_blocks == 0 is passed to any region
 * function or when the verifier does not specify one for the session.  The
 * verifier can override it per session to trade off graph depth (security
 * margin) against memory bandwidth (performance).
 */
#define POSE_CHUNK_BLOCKS 4096

/*
 * Number of chunks pose_graph_label_region processes in parallel.
 *
 * Callers that feed pose_graph_label_region one chunk at a time (e.g. disk
 * labeling with its own outer loop) will see no parallelism — they should
 * instead pass POSE_REGION_THREADS * POSE_CHUNK_BLOCKS blocks per call so
 * the function can saturate all available cores.
 */
#ifndef POSE_REGION_THREADS
#define POSE_REGION_THREADS 4
#endif

/*
 * Compute graph_parameter_n from m.
 * n = max(0, bit_length(m-1) - 1) for m > 0.
 * This is the smallest n such that 2^(n+1) >= m.
 */
int pose_graph_parameter_n(uint64_t m);

/*
 * Label m blocks.
 *
 * Computes all node labels for the pose-db-drg-v1 graph, then writes the
 * m challenge-node labels into out[0..m-1][POSE_HASH_BYTES].
 *
 * Parameters:
 *   seed      - verifier session seed
 *   seed_len  - seed length in bytes
 *   m         - number of output blocks (>= 1)
 *   out       - m * POSE_HASH_BYTES bytes of output (caller allocates)
 *
 * Returns 0 on success, -1 if m exceeds POSE_GRAPH_MAX_M or
 * allocation fails.
 */
int pose_graph_label(const uint8_t *seed, size_t seed_len,
                     uint64_t m, uint8_t *out,
                     pose_hash_algo_t algo);

/*
 * Recompute the label for a single challenge index (verifier use).
 *
 * challenge_idx must be in [0, m).  The returned label can be compared
 * against the prover's stored label.
 *
 * Returns 0 on success, -1 on error.
 */
int pose_graph_challenge(const uint8_t *seed, size_t seed_len,
                         uint64_t m, uint64_t challenge_idx,
                         uint8_t out[POSE_HASH_BYTES],
                         pose_hash_algo_t algo);

/*
 * Label a contiguous memory region in chunk_blocks-block chunks.
 *
 * region_len must be a multiple of POSE_HASH_BYTES.  The region pointer
 * may be a /dev/mem mmap — labels are written directly into it.
 *
 * chunk_blocks controls graph depth per chunk (security/performance trade-off):
 *   - 0 means use the POSE_CHUNK_BLOCKS compile-time default.
 *   - Must be a power of two in [1, POSE_GRAPH_MAX_M].
 *   - Larger values increase γ (depth), hardening against fast-phase relay
 *     attacks; smaller values reduce peak scratch memory and labeling latency.
 *
 * Each chunk i uses a seed derived via pose_chunk_seed(session_seed,
 * seed_len, region_block_offset / chunk_blocks + i).
 * region_block_offset is the chunk-aligned block index of region[0] within
 * the full physical address space (allows the verifier to reconstruct the
 * per-chunk seed for any challenged block).
 *
 * Returns 0 on success, -1 on error.
 */
int pose_graph_label_region(const uint8_t *session_seed, size_t seed_len,
                             void *region, size_t region_len,
                             uint64_t region_block_offset,
                             pose_hash_algo_t algo,
                             uint32_t chunk_blocks);

/*
 * Returns the per-thread scratch buffer size in bytes needed by
 * pose_graph_label_region_ex for the given chunk_blocks.
 * chunk_blocks == 0 uses the POSE_CHUNK_BLOCKS default.
 */
size_t pose_graph_scratch_bytes(uint32_t chunk_blocks);

/*
 * In-place variants — paper-faithful: only the output set O(G) is persisted.
 *
 * The full depth-robust scaffold is computed transiently in a bounded label
 * arena; only the m = chunk_blocks output-set labels are written to physical
 * memory, in challenge-rank order.  Every persisted byte is therefore a
 * challengeable output-set label (Corollary 2 of the PoSE-DB paper holds only
 * for O(G)), so the attested fraction of the persisted region is 1.0.  The
 * scaffold costs scratch_node_count(cb)/cb ≈ 130-384× more hashing than the
 * outputs alone (the wipe-time price of full attestation).
 *
 * pose_graph_scratch_bytes_inplace — per-thread scratch: two base-output arrays
 *   plus the transient label arena (POSE_ARENA_FACTOR·half_w 32-byte slots).
 *   This is proportional to chunk_blocks, never to the region.
 *
 * pose_graph_super_chunk_blocks — physical block count per in-place super-chunk;
 *   now exactly chunk_blocks (the persisted output set).
 *
 * pose_graph_challenge_node_ids — fill node_ids_out[0..chunk_blocks-1] with the
 *   physical block index of each challenge rank.  This is now the identity
 *   (rank r persisted at block r); the scratch argument is unused but kept for
 *   ABI stability.
 *
 * pose_graph_label_region_pooled_inplace — declared after pose_graph_pool_t below.
 */
size_t   pose_graph_scratch_bytes_inplace(uint32_t chunk_blocks);
uint64_t pose_graph_super_chunk_blocks(uint32_t chunk_blocks);
void     pose_graph_challenge_node_ids(uint32_t chunk_blocks,
                                        uint64_t *node_ids_out, uint8_t *scratch);

/*
 * Total number of nodes in the full depth-robust scaffold of one chunk
 * (= the transient working set computed but NOT persisted by the faithful
 * in-place prover).  The GPU labeler uses this to size its transient HBM work
 * buffer: one super-chunk's scaffold occupies
 * pose_graph_scaffold_node_count(cb) × POSE_HASH_BYTES bytes.
 */
uint64_t pose_graph_scaffold_node_count(uint32_t chunk_blocks);

/*
 * Like pose_graph_label_region but uses caller-supplied scratch buffers.
 *
 * scratch must be an array of POSE_REGION_THREADS pointers, each pointing to
 * at least pose_graph_scratch_bytes(chunk_blocks) bytes of writable memory
 * that does not overlap the region being labeled.
 */
int pose_graph_label_region_ex(const uint8_t *session_seed, size_t seed_len,
                                void *region, size_t region_len,
                                uint64_t region_block_offset,
                                uint8_t *scratch[POSE_REGION_THREADS],
                                pose_hash_algo_t algo,
                                uint32_t chunk_blocks);

/*
 * Reusable thread pool for chunk-parallel labeling.
 *
 * Create the pool (which starts POSE_REGION_THREADS-1 worker threads) once,
 * before labeling begins, and reuse it for every region.  Without the pool,
 * pose_graph_label_region_ex spawns thousands of short-lived pthreads over a
 * large region, which is slow and, when the region is live physical memory,
 * lets the kernel allocate thread stacks inside the range being overwritten.
 */
typedef struct pose_graph_pool pose_graph_pool_t;

/* Allocate and start the pool.  Returns NULL on error. */
pose_graph_pool_t *pose_graph_pool_create(void);

/* Stop workers and free the pool. */
void pose_graph_pool_destroy(pose_graph_pool_t *pool);

/*
 * Like pose_graph_label_region_ex but dispatches chunk-parallel work to a
 * pre-created thread pool rather than spawning new threads per batch.
 */
int pose_graph_label_region_pooled(const uint8_t *session_seed, size_t seed_len,
                                    void *region, size_t region_len,
                                    uint64_t region_block_offset,
                                    uint8_t *scratch[POSE_REGION_THREADS],
                                    pose_graph_pool_t *pool,
                                    pose_hash_algo_t algo,
                                    uint32_t chunk_blocks);

/*
 * In-place variant: only the m = chunk_blocks output-set labels are written to
 * physical memory, in challenge-rank order (rank r at block r).  Each chunk
 * occupies pose_graph_super_chunk_blocks(chunk_blocks) = chunk_blocks physical
 * blocks; the scaffold stays transient in scratch and is never persisted, so
 * 100% of the persisted region is attestable.
 * scratch[i] must be pose_graph_scratch_bytes_inplace(chunk_blocks) bytes each.
 */
int pose_graph_label_region_pooled_inplace(
        const uint8_t *session_seed, size_t seed_len,
        void *region, size_t region_len,
        uint64_t region_block_offset,
        uint8_t *scratch[POSE_REGION_THREADS],
        pose_graph_pool_t *pool,
        pose_hash_algo_t algo,
        uint32_t chunk_blocks);

/*
 * Write-through sink: called once per labeled super-chunk with the labeled
 * buffer (buf), the number of bytes to commit (len), and the absolute byte
 * offset of this super-chunk in the target (off).  Must return 0 on success,
 * non-zero on failure (which aborts the wave).  Invoked concurrently from
 * worker threads, each with a distinct buf/off, so the implementation must be
 * thread-safe (independent offsets on one fd are fine).
 */
typedef int (*pose_label_sink_fn)(void *ctx, const uint8_t *buf,
                                  size_t len, uint64_t off);

/*
 * Label one wave of up to POSE_REGION_THREADS super-chunks in place, each into
 * its own slot buffer, streaming each labeled super-chunk to `sink` from the
 * worker that produced it.  This bounds peak RAM at POSE_REGION_THREADS ×
 * slot_bytes instead of buffering the whole region, and lets the I/O overlap
 * with sibling workers' labeling.
 *
 *   slots[i]            per-worker output buffer, >= one super-chunk
 *                       (pose_graph_super_chunk_blocks × POSE_HASH_BYTES) and
 *                       >= write_len.  Reused across waves; pre-zero any padding
 *                       in [super_bytes, write_len) once before the first wave.
 *   scratch[i]          per-worker scratch (pose_graph_scratch_bytes_inplace).
 *   n_this              super-chunks in this wave, 1..POSE_REGION_THREADS.
 *   super_index_base    global super-chunk index of slots[0].
 *   region_block_offset byte_start / POSE_HASH_BYTES (matches the chunk-seed
 *                       derivation used elsewhere so the verifier agrees).
 *   byte_start          absolute byte offset of super-chunk 0 in the target.
 *   write_len           bytes committed per super-chunk (sink offset for slot i
 *                       is byte_start + (super_index_base + i) * write_len).
 *
 * Returns 0 on success, non-zero if labeling or the sink failed.
 */
int pose_graph_label_disk_wave(
        const uint8_t *session_seed, size_t seed_len,
        uint8_t *const slots[POSE_REGION_THREADS],
        uint8_t *const scratch[POSE_REGION_THREADS],
        pose_graph_pool_t *pool,
        pose_hash_algo_t algo,
        uint32_t chunk_blocks,
        int n_this,
        uint64_t super_index_base,
        uint64_t region_block_offset,
        uint64_t byte_start,
        size_t write_len,
        pose_label_sink_fn sink,
        void *sink_ctx);

/* ── Edge-structure API (visualization / debugging) ──────────────────────── */

/*
 * Per-node edge descriptor returned by pose_graph_edges().
 *
 *  num_preds   0 = source node (no predecessors)
 *              1 = one predecessor in pred[0]
 *              2 = two predecessors in pred[0] and pred[1]
 *  pred[]      node IDs of predecessors (lower-ID pred first for num_preds==2)
 *  challenge_rank
 *              -1 = internal node (not in the challenge output set)
 *              >= 0 = index of this node in the challenge output [0, m)
 */
typedef struct {
    int      num_preds;
    uint64_t pred[2];
    int      challenge_rank;
} pose_node_t;

/*
 * Enumerate all nodes and their predecessor edges for a graph of m blocks.
 *
 *   *nodes_out   allocated array of pose_node_t; caller must free()
 *   *count_out   total node count (>= m; includes all scratch nodes)
 *
 * Does not compute any hash values — purely structural (fast even for large m).
 *
 * Returns 0 on success, -1 on m out of range or allocation failure.
 */
int pose_graph_edges(uint64_t m, pose_node_t **nodes_out, uint64_t *count_out);

/*
 * Seed-independent scratch slot-recycling map for the GPU per-chunk labeling
 * strategies (BLOCK / THREAD).  Depends only on chunk_blocks, so it is computed
 * once and reused for every super-chunk and every GPU — exactly like the
 * topology from pose_graph_edges().
 *
 * For each scaffold node id in [0, pose_graph_scaffold_node_count(chunk_blocks)):
 *   slot_out[id]   = recycled scratch slot index in [0, *pool_size_out); a node's
 *                    transient label lives at scratch[base + slot_out[id]*32].
 *   *pool_size_out = peak number of simultaneously-live slots (the per-chunk
 *                    scratch size, in 32-byte labels).
 *
 * EVERY node — including output-set nodes — is assigned a real recycled slot:
 * output nodes are read by later scaffold nodes (they are NOT pure sinks), so
 * they must stay readable from scratch.  A per-chunk kernel additionally copies
 * each output node to the persisted region at the moment it is computed (using
 * the challenge_rank from pose_graph_edges), before its slot can be recycled.
 *
 * Liveness is computed at *dependency-level* granularity: a node's slot is freed
 * only after the last level that reads it completes, and every node in a level
 * gets a distinct slot.  This makes the map safe for parallel within-level
 * labeling (BLOCK, where a level's nodes are written concurrently) and therefore
 * also for fully sequential topological labeling (THREAD).  The hash inputs,
 * node order, keys, and descriptor are unchanged from the CPU/LEVEL path — only
 * the byte offset each transient label is stored at differs — so the persisted
 * output labels remain byte-identical and the verifier needs no change.
 *
 *   slot_out must point to pose_graph_scaffold_node_count(chunk_blocks) uint32_t.
 *
 * Returns 0 on success, -1 on chunk_blocks out of range or allocation failure.
 */
int pose_graph_slot_map(uint32_t chunk_blocks,
                        uint32_t *slot_out,
                        uint32_t *pool_size_out);

#ifdef __cplusplus
}
#endif

#endif /* POSE_GRAPH_H */
