/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

#ifndef POSE_GPU_LABEL_H
#define POSE_GPU_LABEL_H

#include "hash.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * GPU HBM graph labeling (CUDA).  Labels a device buffer in place with the
 * PoSE-DB graph labeling, byte-identical to the CPU in-place path
 * (pose_graph_label_region_pooled_inplace) so the verifier can challenge it
 * with the same common/graph.c FFI it uses for RAM.
 *
 *   session_seed/seed_len   verifier session seed
 *   chunk_blocks            graph depth per chunk (0 = POSE_CHUNK_BLOCKS)
 *   hbm_dev_ptr             cudaMalloc'd device buffer on the CURRENT device
 *   hbm_len                 buffer length in bytes
 *   region_block_offset     block offset of hbm_dev_ptr[0] in the chunk-index
 *                           space (normally 0 for a fresh GPU buffer)
 *   algo                    must be POSE_HASH_BLAKE3
 *
 * Layout produced (paper-faithful): each super-chunk j of super_bytes =
 * pose_graph_super_chunk_blocks(chunk_blocks)*32 == chunk_blocks*32 bytes holds
 * ONLY the output set O(G) in challenge-rank order — output rank r's 32-byte
 * label is at hbm[j*super_bytes + r*32].  The full scaffold is computed
 * transiently in a separate HBM work buffer (sized to a wave of scaffolds),
 * which is zeroed and freed before this call returns, so every persisted byte is
 * a challengeable output-set label (100% attested).  Tail bytes smaller than one
 * super-chunk are left untouched (mirrors the CPU floor).
 *
 * Returns 0 on success, -1 on error (bad algo, allocation failure, CUDA error).
 */
int pose_graph_label_hbm(const uint8_t *session_seed, size_t seed_len,
                         uint32_t chunk_blocks,
                         void *hbm_dev_ptr, size_t hbm_len,
                         uint64_t region_block_offset,
                         pose_hash_algo_t algo);

/*
 * Reusable-topology variant of pose_graph_label_hbm.
 *
 * The scaffold topology (host-side graph construction + dependency levels + the
 * H2D upload) is seed-independent — it depends only on chunk_blocks — but is the
 * dominant fixed cost of a label call.  A caller that labels many HBM buffers
 * (e.g. a disk wipe streaming windows to the device) can build it ONCE and reuse
 * it across every window instead of paying that cost per call.
 *
 * The topology holds device memory and is bound to the CURRENT CUDA device, so
 * build/label/free must all run with the same cudaSetDevice as the buffers.
 * Labels produced are byte-identical to pose_graph_label_hbm (which is now just
 * build -> label -> free), so verification against the CPU path is unaffected.
 *
 *   pose_graph_hbm_topo_build  builds + uploads the topology for chunk_blocks
 *                              (0 = POSE_CHUNK_BLOCKS).  algo must be BLAKE3.
 *                              Returns NULL on error.
 *   pose_graph_hbm_topo_label  labels hbm_dev_ptr[0..hbm_len) in place using the
 *                              resident topology; region_block_offset places the
 *                              buffer in the chunk-index space (see
 *                              pose_graph_label_hbm).  Returns 0 (incl. a buffer
 *                              smaller than one super-chunk, which labels
 *                              nothing) or -1 on error.  Reentrant per topology
 *                              only if serialized — call it from one thread.
 *   pose_graph_hbm_topo_free   frees the device topology and the handle.
 */
typedef struct pose_hbm_topo pose_hbm_topo_t;

pose_hbm_topo_t *pose_graph_hbm_topo_build(uint32_t chunk_blocks,
                                           pose_hash_algo_t algo);

int pose_graph_hbm_topo_label(const pose_hbm_topo_t *topo,
                              const uint8_t *session_seed, size_t seed_len,
                              void *hbm_dev_ptr, size_t hbm_len,
                              uint64_t region_block_offset);

void pose_graph_hbm_topo_free(pose_hbm_topo_t *topo);

/*
 * Self-test helper: compute one node label on the GPU for explicit inputs.
 * Mirrors pose_label_node (BLAKE3) so a known-answer test can compare GPU vs
 * CPU output byte-for-byte.  pred0/pred1 may be NULL (treated as zero blocks).
 * key_src/key_int are the 8-word keys from pose_label_node_keys.
 *
 * Returns 0 on success, -1 on CUDA error.
 */
int pose_gpu_label_node_single(uint8_t out[32], int is_source,
                               uint64_t node_index,
                               const uint8_t *pred0, const uint8_t *pred1,
                               const uint8_t *seed, const uint8_t *descriptor,
                               const uint32_t key_src[8],
                               const uint32_t key_int[8]);

#ifdef __cplusplus
}
#endif

#endif /* POSE_GPU_LABEL_H */
