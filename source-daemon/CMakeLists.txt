# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

cmake_minimum_required(VERSION 3.16)
project(source_daemon C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# GPU (HBM) targets: on by default on every architecture (an x86 host with
# H200s, a Grace host with Blackwells).  Needs the CUDA toolkit (nvcc) at build
# time only; the daemon links cudart statically and dlopen()s the driver's
# libcuda.so.1 at run time.  Deliberately strict: a build host without nvcc is
# an error here, not a silent CPU-only daemon; pass -DTARGET_GPU=OFF for one
# (the native loopback-test builds on a dev VM do).  pose-common's option has
# the same name, so the value carries through to it.
option(TARGET_GPU "GPU HBM targets (CUDA; requires nvcc)" ON)
if(TARGET_GPU)
    include(CheckLanguage)
    check_language(CUDA)
    if(NOT CMAKE_CUDA_COMPILER)
        message(FATAL_ERROR
            "TARGET_GPU is ON but no CUDA compiler (nvcc) was found. Build in the "
            "pose-build container, or configure with -DTARGET_GPU=OFF for a daemon "
            "without GPU targets.")
    endif()
endif()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pose-sink pose-sink)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pose-common pose-common)

add_executable(source-daemon
    src/source_main.c
    src/downstream.c
    src/label_gen.c
    src/gpu_targets.c
    src/gpu_sessions.c
)
target_link_libraries(source-daemon PRIVATE pose-sink pose_common)
if(TARGET_GPU)
    target_compile_definitions(source-daemon PRIVATE TARGET_GPU=1)
    # The CUDA library is C++; keep its runtime out of the initramfs.
    target_link_libraries(source-daemon PRIVATE pose_common_cuda -static-libstdc++ -static-libgcc)
endif()
target_compile_options(source-daemon PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(source-daemon PRIVATE _GNU_SOURCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pose-runtime.cmake)
if(TARGET_GPU)
    # The static cudart dlopen()s the driver's libcuda.so.1 (packed into the
    # image from vendor/nvidia-libcuda by the UKI script), which needs these
    # from glibc beyond the daemon's own libc.
    pose_daemon_link(source-daemon libpthread.so.0 libm.so.6 libdl.so.2 librt.so.1)
else()
    pose_daemon_link(source-daemon)
endif()
