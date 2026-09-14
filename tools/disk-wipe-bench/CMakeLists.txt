# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

cmake_minimum_required(VERSION 3.18)
project(disk_wipe_bench CXX CUDA)

# GPU-driven PoSE-DB disk wipe + throughput benchmark.  Reuses the validated
# label generator (pose_common_cuda / pose_common) — no algorithm here, only the
# host-side D2H + O_DIRECT write plumbing and multi-GPU scheduling.
#
# Build via the repo Makefile:  make disk-wipe-bench   (in the CUDA container)
# or standalone on a CUDA host:
#   cmake -S tools/disk-wipe-bench -B tools/disk-wipe-bench/build \
#         -DCMAKE_BUILD_TYPE=Release -DPOSE_CUDA_ARCH=90
#   cmake --build tools/disk-wipe-bench/build -j

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# CUDA 13's libcu++ requires C++17; keep the .cu on the same dialect as common/.
set(CMAKE_CUDA_STANDARD 17)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

# Compute capability for the target GPU(s): 90 = Hopper (H100/H200),
# 100 = Blackwell (GB200).  Forwarded to common/ so both halves agree.
set(POSE_CUDA_ARCH "90" CACHE STRING "CUDA compute capability for the target GPU(s)")

# Turn on the CUDA labeling library inside common/ and pick the labeling
# strategy (level = byte-parity reference / default; block = recommended once
# validated; thread = measure-only; warp = one warp per chunk, no block barrier).
# These must be set before add_subdirectory.
set(TARGET_GPU ON CACHE BOOL "" FORCE)
set(POSE_GPU_LABEL_STRATEGY "level" CACHE STRING "GPU labeling strategy: level, block, thread, warp")
set(POSE_GPU_WARP_MIN_BLOCKS "3" CACHE STRING "warp strategy: min blocks/SM for __launch_bounds__")

add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common
    ${CMAKE_CURRENT_BINARY_DIR}/common
)

find_package(CUDAToolkit REQUIRED)
find_package(Threads REQUIRED)

add_executable(disk-wipe-bench disk_wipe_bench.cu)

set_target_properties(disk-wipe-bench PROPERTIES
    CUDA_RESOLVE_DEVICE_SYMBOLS ON
    CUDA_ARCHITECTURES "${POSE_CUDA_ARCH}"
)

target_link_libraries(disk-wipe-bench PRIVATE
    pose_common_cuda
    pose_common
    CUDA::cudart
    Threads::Threads
)

message(STATUS "disk-wipe-bench: CUDA arch ${POSE_CUDA_ARCH}, "
               "labeling strategy ${POSE_GPU_LABEL_STRATEGY}")
