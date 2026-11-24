/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * blake3_node.cu — known-answer-test entry point for the on-device keyed
 * BLAKE3 node hash.  The labeling kernels live in label_gpu_*.cu; this TU
 * exposes a single-node helper so a test can verify GPU output matches the
 * CPU pose_label_node byte-for-byte.
 */

#include "blake3_node.cuh"
#include "../../include/gpu_label.h"

#include <cuda_runtime.h>

namespace {

__global__ void k_label_single(uint8_t *out, int is_source, uint64_t node_index,
                               const uint8_t *pred0, const uint8_t *pred1,
                               const uint8_t *seed, const uint8_t *descriptor,
                               const uint32_t *key_src, const uint32_t *key_int)
{
    if (threadIdx.x != 0 || blockIdx.x != 0) return;
    const uint32_t *key = is_source ? key_src : key_int;
    pose_gpu::label_node(out, key, node_index,
                         is_source ? nullptr : pred0,
                         is_source ? nullptr : pred1,
                         seed, descriptor);
}

} /* namespace */

extern "C" int pose_gpu_label_node_single(uint8_t out[32], int is_source,
                                           uint64_t node_index,
                                           const uint8_t *pred0,
                                           const uint8_t *pred1,
                                           const uint8_t *seed,
                                           const uint8_t *descriptor,
                                           const uint32_t key_src[8],
                                           const uint32_t key_int[8])
{
    uint8_t   *d_out  = nullptr;
    uint8_t   *d_p0   = nullptr, *d_p1 = nullptr, *d_seed = nullptr, *d_desc = nullptr;
    uint32_t  *d_ksrc = nullptr, *d_kint = nullptr;
    int rc = -1;

    if (cudaMalloc(&d_out, 32) != cudaSuccess) goto done;
    if (cudaMalloc(&d_seed, 32) != cudaSuccess) goto done;
    if (cudaMalloc(&d_desc, 32) != cudaSuccess) goto done;
    if (cudaMalloc(&d_ksrc, 32) != cudaSuccess) goto done;
    if (cudaMalloc(&d_kint, 32) != cudaSuccess) goto done;
    if (pred0 && cudaMalloc(&d_p0, 32) != cudaSuccess) goto done;
    if (pred1 && cudaMalloc(&d_p1, 32) != cudaSuccess) goto done;

    cudaMemcpy(d_seed, seed, 32, cudaMemcpyHostToDevice);
    cudaMemcpy(d_desc, descriptor, 32, cudaMemcpyHostToDevice);
    cudaMemcpy(d_ksrc, key_src, 32, cudaMemcpyHostToDevice);
    cudaMemcpy(d_kint, key_int, 32, cudaMemcpyHostToDevice);
    if (pred0) cudaMemcpy(d_p0, pred0, 32, cudaMemcpyHostToDevice);
    if (pred1) cudaMemcpy(d_p1, pred1, 32, cudaMemcpyHostToDevice);

    k_label_single<<<1, 1>>>(d_out, is_source, node_index, d_p0, d_p1,
                             d_seed, d_desc, d_ksrc, d_kint);
    if (cudaDeviceSynchronize() != cudaSuccess) goto done;

    if (cudaMemcpy(out, d_out, 32, cudaMemcpyDeviceToHost) != cudaSuccess) goto done;
    rc = 0;

done:
    cudaFree(d_out);  cudaFree(d_p0);   cudaFree(d_p1);
    cudaFree(d_seed); cudaFree(d_desc); cudaFree(d_ksrc); cudaFree(d_kint);
    return rc;
}
