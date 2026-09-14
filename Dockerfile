# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

# CUDA build image for disk-wipe-bench and the pose_common_cuda labeler.
#
# Based on the NVIDIA CUDA devel image so nvcc + the CUDA runtime are
# available.  Pinned to linux/amd64: the intended targets are x86-64 Linux
# hosts with discrete NVIDIA GPUs.  On an Apple Silicon Mac this runs under
# emulation (slow, but produces a correct x86-64 binary); on the GPU host it
# runs natively, or skip Docker entirely and use the host's nvcc.
#
#   docker build -t pose-build-cuda .
#   make disk-wipe-bench          # wraps the cmake calls in this container
FROM --platform=linux/amd64 nvidia/cuda:12.6.2-devel-ubuntu24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc \
    g++ \
    cmake \
    make \
    ninja-build \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
