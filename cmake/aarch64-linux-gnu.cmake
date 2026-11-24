# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

# CMake toolchain for 64-bit ARM: the BlueField-3 DPU's Arm cores, and other
# 64 bit Arm hosts.  On an arm64 build host this is the native compiler under
# its triplet name; on an amd64 build host it is the cross compiler.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_ASM_COMPILER aarch64-linux-gnu-gcc)

# No -mcpu: the compiler's armv8-a baseline is deliberate, so one binary runs on
# the DPU and on any arm64 machine used for testing.

# CMAKE_LIBRARY_ARCHITECTURE steers find_library at the arm64 multiarch directory
# so it cannot latch onto an amd64 build host's libraries
set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)

# CUDA (pose-common's TARGET_GPU): nvcc is the build host's own (x86_64 or
# sbsa), compiling device code for either target; it cross-compiles the host
# side with this triplet's g++ and takes the target's cudart/headers from
# /usr/local/cuda/targets/sbsa-linux (docker/Dockerfile unpacks the non-native
# target tree).  Harmless when no CUDA language is enabled.
set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)
set(CMAKE_CUDA_HOST_COMPILER aarch64-linux-gnu-g++)
set(CMAKE_CUDA_FLAGS_INIT "-target-dir sbsa-linux")
set(CUDAToolkit_ROOT /usr/local/cuda)
set(CUDAToolkit_TARGET_DIR /usr/local/cuda/targets/sbsa-linux)
