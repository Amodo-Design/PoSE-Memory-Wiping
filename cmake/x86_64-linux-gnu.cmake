# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

# CMake toolchain for the x86_64 host.  On an amd64 build host this is the
# native compiler under its triplet name; on an arm64 build host it is the
# cross compiler.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER x86_64-linux-gnu-g++)
set(CMAKE_ASM_COMPILER x86_64-linux-gnu-gcc)

# x86-64-v2 (A.K.A assume we have at least: SSE4.2, POPCNT, CMPXCHG16B)
set(POSE_X86_64_ISA_FLAGS "-march=x86-64-v2")
set(CMAKE_C_FLAGS_INIT   "${POSE_X86_64_ISA_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${POSE_X86_64_ISA_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${POSE_X86_64_ISA_FLAGS}")

# CMAKE_LIBRARY_ARCHITECTURE steers find_library at the amd64 multiarch directory
# so it cannot latch onto an arm64 build host's libraries
set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# CUDA (pose-common's TARGET_GPU): nvcc is the build host's own (x86_64 or
# sbsa), compiling device code for either target; it cross-compiles the host
# side with this triplet's g++ and takes the target's cudart/headers from
# /usr/local/cuda/targets/x86_64-linux (docker/Dockerfile unpacks the non-native
# target tree).  Harmless when no CUDA language is enabled.
set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)
set(CMAKE_CUDA_HOST_COMPILER x86_64-linux-gnu-g++)
set(CMAKE_CUDA_FLAGS_INIT "-target-dir x86_64-linux")
set(CUDAToolkit_ROOT /usr/local/cuda)
set(CUDAToolkit_TARGET_DIR /usr/local/cuda/targets/x86_64-linux)
