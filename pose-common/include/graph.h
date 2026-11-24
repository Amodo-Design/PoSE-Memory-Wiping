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
 * is stored.  These two keep every node's label in a full scratch buffer
 * (2·S(n+1) labels); the in-place variants further down keep only a bounded
 * working set.
 *
 * POSE_GRAPH_MAX_M is the largest m (chunk_blocks) any labeler accepts:
 * 2^20 = 1,048,576 blocks.
 */

#define POSE_GRAPH_MAX_M (1u << 20)

/*
 * Default blocks per chunk for pose_graph_label_region.
 *
 * Each chunk is labeled independently with a derived seed.
 * Chunk size = POSE_CHUNK_BLOCKS * POSE_HASH_BYTES = 4096 * 32 = 128 KB.
 * Per-thread scratch at this size is about 17 MiB for the full-scratch
 * labelers (2 * S(12) labels) and 1.25 MiB for the in-place ones — constant
 * regardless of total region size.
 *
 * This default is used when chunk_blocks == 0 is passed to any region
 * function.  The verifier sets chunk_blocks per session to trade off graph
 * depth (security margin) against memory bandwidth (performance).
 */
#define POSE_CHUNK_BLOCKS 4096

/*
 * Upper bound on the number of chunks the region labelers process in
 * parallel (one CPU labeling thread per chunk).
 *
 * The thread count actually used is decided at runtime by
 * pose_region_threads(): the number of CPUs this process may run on, clamped
 * to [1, POSE_REGION_THREADS_MAX].  The compile-time maximum only sizes the
 * fixed per-thread arrays (scratch pointers, pool slots) and caps the peak
 * scratch footprint (pose_graph_scratch_bytes_inplace × threads) on
 * many-core hosts; it does not by itself cost RAM.
 *
 * Callers that feed pose_graph_label_region one chunk at a time (e.g. disk
 * labeling with its own outer loop) will see no parallelism — they should
 * instead pass pose_region_threads() * POSE_CHUNK_BLOCKS blocks per call so
 * the function can saturate all available cores.
 */
#ifndef POSE_REGION_THREADS_MAX
#define POSE_REGION_THREADS_MAX 64
#endif

#if POSE_REGION_THREADS_MAX < 1
#error "POSE_REGION_THREADS_MAX must be >= 1"
#endif

/*
 * Number of CPUs this process is allowed to run on (affinity mask on Linux,
 * else the online CPU count), or 1 if it cannot be determined.
 */
int pose_region_cpus_online(void);

/*
 * Number of CPU labeling threads the region labelers and pose_graph_pool_create
 * use: min(pose_region_cpus_online(), POSE_REGION_THREADS_MAX), at least 1.
 * Computed once on first call and cached.  Callers must allocate exactly this
 * many scratch (and slot) buffers — indices [0, pose_region_threads()) of the
 * POSE_REGION_THREADS_MAX-sized arrays passed to the labelers are used.
 */
int pose_region_threads(void);

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

/*
 * Label one chunk in place on the calling thread: the chunk_blocks output-set
 * labels of the chunk with global index chunk_index (its seed is
 * pose_chunk_seed(session_seed, chunk_index)), rank r at out[r].  The same
 * labels the region labelers store for that chunk, which for a region at byte
 * base puts chunk (base / 32) / chunk_blocks + j at the region's j-th
 * super-chunk.  out is chunk_blocks * POSE_HASH_BYTES bytes; scratch is
 * pose_graph_scratch_bytes_inplace(chunk_blocks) bytes and private to the
 * caller, so independent calls may run on as many threads as the caller
 * likes (the verifier recomputes challenged chunks this way).
 * chunk_blocks 0 means POSE_CHUNK_BLOCKS.  Returns 0, or -1 on bad arguments.
 */
int pose_graph_label_chunk_inplace(const uint8_t *session_seed, size_t seed_len,
                                   uint64_t chunk_index, uint8_t *out, uint8_t *scratch,
                                   pose_hash_algo_t algo, uint32_t chunk_blocks);
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
 * scratch must be an array of POSE_REGION_THREADS_MAX pointers, of which the
 * first pose_region_threads() are used; each of those must point to
 * at least pose_graph_scratch_bytes(chunk_blocks) bytes of writable memory
 * that has been faulted in before memory_map_post_mmap_rescan runs — so
 * those physical pages are in the daemon's skip set and cannot alias with
 * the labeled region.
 */
int pose_graph_label_region_ex(const uint8_t *session_seed, size_t seed_len,
                                void *region, size_t region_len,
                                uint64_t region_block_offset,
                                uint8_t *scratch[POSE_REGION_THREADS_MAX],
                                pose_hash_algo_t algo,
                                uint32_t chunk_blocks);

/*
 * Reusable thread pool for chunk-parallel labeling.
 *
 * Create the pool (which starts pose_region_threads()-1 worker threads) BEFORE
 * calling memory_map_post_mmap_rescan.  The workers' kernel stack physical
 * pages are allocated from the buddy allocator during pthread_create; the
 * subsequent rescan marks those pages as non-buddy skip pages.  Without the
 * pool, pose_graph_label_region_ex would spawn ~11,000 short-lived pthreads
 * during labeling, each causing the kernel to allocate a new stack from buddy
 * pages that are in the labeled region — corrupting live kernel data structures.
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
                                    uint8_t *scratch[POSE_REGION_THREADS_MAX],
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
        uint8_t *scratch[POSE_REGION_THREADS_MAX],
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
 * Label one wave of up to pose_region_threads() super-chunks in place, each into
 * its own slot buffer, streaming each labeled super-chunk to `sink` from the
 * worker that produced it.  This bounds peak RAM at pose_region_threads() ×
 * slot_bytes instead of buffering the whole region, and lets the I/O overlap
 * with sibling workers' labeling.
 *
 *   slots[i]            per-worker output buffer, >= one super-chunk
 *                       (pose_graph_super_chunk_blocks × POSE_HASH_BYTES) and
 *                       >= write_len.  Reused across waves; pre-zero any padding
 *                       in [super_bytes, write_len) once before the first wave.
 *   scratch[i]          per-worker scratch (pose_graph_scratch_bytes_inplace).
 *   n_this              super-chunks in this wave, 1..pose_region_threads().
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
        uint8_t *const slots[POSE_REGION_THREADS_MAX],
        uint8_t *const scratch[POSE_REGION_THREADS_MAX],
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
    uint32_t pred[2];          /* node ids fit 32 bits: the scaffold of the largest
                                  chunk_blocks is ~150 M nodes (checked by callers) */
    int32_t  challenge_rank;
    uint8_t  num_preds;
} pose_node_t;                 /* 16 bytes: this array is the bulk of a topology
                                  build's host memory (~79 M nodes at 2^18) */

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

/*
 * The pieces pose_graph_slot_map is made of, on structure-of-arrays input
 * (np / p0 / p1 per node, as the GPU topology holds them), so a caller that
 * already has those arrays — the GPU topology build — need not regenerate
 * the graph and can free each array as soon as it is consumed.  Host memory
 * is what bounds the topology build (the x86 host keeps only a few GiB), so
 * the sequence matters: extract the arrays from pose_graph_edges and free
 * the nodes; levels; level order; last use; upload np/p0/p1/order and free
 * them; then the slot pass, which needs only level, last_use and the orders.
 *
 *   pose_graph_levels       dependency level per node (every predecessor has
 *                           a lower id); *num_levels_out = max level + 1.
 *   pose_graph_level_order  counting-sort of node ids by level: order_out
 *                           [count], lvl_start_out [num_levels + 1].
 *   pose_graph_last_use     the last level that reads each node (its own
 *                           level for a sink).
 *   pose_graph_slot_map_from  the slot map itself from level, last_use and
 *                           the level order; pool_size_out = peak live slots.
 *
 * All return 0, or -1 on allocation failure.
 */
int pose_graph_levels(uint64_t count, const uint8_t *np, const uint32_t *p0, const uint32_t *p1,
                      int *level_out, int *num_levels_out);
int pose_graph_level_order(uint64_t count, const int *level, int num_levels,
                           uint32_t *order_out, uint32_t *lvl_start_out);
int pose_graph_last_use(uint64_t count, const uint8_t *np, const uint32_t *p0, const uint32_t *p1,
                        const int *level, int *last_use_out);
int pose_graph_slot_map_from(uint64_t count, const int *level, const int *last_use,
                             const uint32_t *lvl_order, const uint32_t *lvl_start, int num_levels,
                             uint32_t *slot_out, uint32_t *pool_size_out);

#ifdef __cplusplus
}
#endif

#endif /* POSE_GRAPH_H */
