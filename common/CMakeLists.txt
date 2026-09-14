# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

cmake_minimum_required(VERSION 3.16)
project(pose_common C)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Target architecture detection ───────────────────────────────────────────
# ARM64: NEON BLAKE3 + ARMv8 crypto extensions for sha256/aes_prf.
# x86_64: BLAKE3 with the vendored SSE/AVX backends.  Output is byte-identical
# to the NEON path (the BLAKE3 spec is implementation-independent), which is what
# label parity with the GPU requires.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(POSE_ARCH_X86 ON)
else()
    set(POSE_ARCH_X86 OFF)
endif()

# Vendor BLAKE3 as a separate object library so -Werror doesn't apply to it.
set(BLAKE3_SOURCES
    vendor/blake3/blake3.c
    vendor/blake3/blake3_dispatch.c
    vendor/blake3/blake3_portable.c
)
if(POSE_ARCH_X86)
    # x86 SIMD backends (intrinsics, BLAKE3 1.8.5).  The runtime dispatcher
    # (blake3_dispatch.c, CPUID) selects the widest the CPU supports; the binary
    # still runs on any x86.  Output is byte-identical to portable/NEON, so GPU
    # label parity is unaffected.
    list(APPEND BLAKE3_SOURCES
        vendor/blake3/blake3_sse2.c
        vendor/blake3/blake3_sse41.c
        vendor/blake3/blake3_avx2.c
        vendor/blake3/blake3_avx512.c
    )
else()
    list(APPEND BLAKE3_SOURCES vendor/blake3/blake3_neon.c)
endif()

add_library(blake3_vendor OBJECT ${BLAKE3_SOURCES})

target_include_directories(blake3_vendor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/blake3
)

if(POSE_ARCH_X86)
    # Each SIMD backend compiles with only its own ISA flag so the others stay
    # callable on CPUs without it (the dispatcher gates them at runtime).
    set_source_files_properties(vendor/blake3/blake3_sse2.c   PROPERTIES COMPILE_OPTIONS "-msse2")
    set_source_files_properties(vendor/blake3/blake3_sse41.c  PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(vendor/blake3/blake3_avx2.c   PROPERTIES COMPILE_OPTIONS "-mavx2")
    set_source_files_properties(vendor/blake3/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
else()
    # ARM64: NEON path; the dispatcher's x86 SIMD branches are compiled out
    # (IS_X86 is false), so the x86 sources are neither built nor referenced.
    target_compile_definitions(blake3_vendor PRIVATE
        BLAKE3_USE_NEON=1
        BLAKE3_NO_AVX512=1
        BLAKE3_NO_AVX2=1
        BLAKE3_NO_SSE41=1
        BLAKE3_NO_SSE2=1
    )
endif()

add_library(pose_common STATIC
    src/hash.c
    src/sha256.c
    src/aes_prf.c
    src/graph.c
    $<TARGET_OBJECTS:blake3_vendor>
)

# sha256.c has a portable C fallback.  aes_prf.c has two hardware backends that
# produce byte-identical labels: ARM AES-CE (-march=armv8-a+crypto) and x86
# AES-NI (-maes).  Without either ISA flag it compiles its abort fallback.  The
# ARMv8 -march flags are only valid on ARM64.
if(POSE_ARCH_X86)
    # AES-NI is present on every x86-64 server CPU (EPYC 9005 included); the
    # aes_prf.c TU is only entered for algo = aes_prf sessions.
    set_source_files_properties(src/aes_prf.c PROPERTIES
        COMPILE_OPTIONS "-maes"
    )
else()
    set_source_files_properties(src/sha256.c PROPERTIES
        COMPILE_OPTIONS "-march=armv8-a+sha2"
    )
    set_source_files_properties(src/aes_prf.c PROPERTIES
        COMPILE_OPTIONS "-march=armv8-a+crypto"
    )
endif()

target_include_directories(pose_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(pose_common PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/blake3
)

if(NOT POSE_ARCH_X86)
    target_compile_definitions(pose_common PRIVATE
        BLAKE3_NO_AVX512=1
        BLAKE3_NO_AVX2=1
        BLAKE3_NO_SSE41=1
        BLAKE3_NO_SSE2=1
    )
endif()

target_compile_options(pose_common PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

target_link_libraries(pose_common PUBLIC Threads::Threads)

# CPU labeling thread count override (blank = header default 4).  Applied as a
# PUBLIC compile definition so every consumer that links pose_common
# (pose_common_cuda, disk-wipe-bench, the tests) sees the same value and
# the scratch[POSE_REGION_THREADS] arrays stay consistent with the worker pool.
set(POSE_REGION_THREADS "" CACHE STRING "CPU labeling thread count (blank = header default 4)")
if(POSE_REGION_THREADS)
    target_compile_definitions(pose_common PUBLIC POSE_REGION_THREADS=${POSE_REGION_THREADS})
endif()

# ── GPU (CUDA) labeling — x86/H200 experiment ───────────────────────────────
# TARGET_GPU=ON enables the CUDA HBM labeling library.  Default OFF so the
# CPU-only ARM and x86 builds never require nvcc.  Sources live under src/cuda/.
option(TARGET_GPU "Build CUDA HBM labeling library (requires CUDA toolkit)" OFF)
if(TARGET_GPU)
    enable_language(CUDA)
    # C++17 minimum: CUDA 13's bundled libcu++ (pulled in via cooperative_groups.h)
    # errors under C++14.  17 is supported by every CUDA we target (11.5+) and does
    # not affect label output.
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
    # Compute capability of the target GPU(s). Default 90 = Hopper (H200/H100);
    # override with -DPOSE_CUDA_ARCH=<sm> (e.g. 100 for Blackwell/GB200, or a
    # semicolon list like "90;100"). Pinning emits native SASS and avoids PTX
    # JIT at launch + the CMP0104 "CUDA_ARCHITECTURES is empty" warning.
    set(POSE_CUDA_ARCH "90" CACHE STRING "CUDA compute capability for the target GPU(s)")
    # GPU labeling parallelization strategy (one strategy translation unit is
    # compiled in alongside the shared host driver label_gpu_common.cu):
    #   level  (default) — full-scaffold wave, level-parallel across super-chunks
    #   block            — one thread block per super-chunk, recycled slot pool
    #   thread           — one thread per super-chunk, recycled slot pool
    #   warp             — one warp per super-chunk, recycled slot pool, __syncwarp()
    #                      between levels (no block barrier; see label_gpu_warp.cu)
    # Selected with -DPOSE_GPU_LABEL_STRATEGY=<name>.
    set(POSE_GPU_LABEL_STRATEGY "level" CACHE STRING "GPU labeling strategy: level, block, thread, warp")
    # warp only: minimum resident blocks/SM requested via __launch_bounds__.
    # 3 matches BLOCK's register-limited occupancy; 4 forces <= 64 regs/thread
    # (check `-Xptxas -v` for zero spills before relying on it).
    set(POSE_GPU_WARP_MIN_BLOCKS "3" CACHE STRING "warp strategy: min blocks/SM for __launch_bounds__")
    add_library(pose_common_cuda STATIC
        src/cuda/blake3_node.cu
        src/cuda/label_gpu_common.cu
    )
    if(POSE_GPU_LABEL_STRATEGY STREQUAL "block")
        target_sources(pose_common_cuda PRIVATE src/cuda/label_gpu_block.cu)
    elseif(POSE_GPU_LABEL_STRATEGY STREQUAL "thread")
        target_sources(pose_common_cuda PRIVATE src/cuda/label_gpu_thread.cu)
    elseif(POSE_GPU_LABEL_STRATEGY STREQUAL "warp")
        target_sources(pose_common_cuda PRIVATE src/cuda/label_gpu_warp.cu)
        target_compile_definitions(pose_common_cuda PRIVATE
            POSE_GPU_WARP_MIN_BLOCKS=${POSE_GPU_WARP_MIN_BLOCKS})
    else()
        target_sources(pose_common_cuda PRIVATE src/cuda/label_gpu_level.cu)
    endif()
    message(STATUS "pose_common_cuda GPU labeling strategy: ${POSE_GPU_LABEL_STRATEGY}")
    set_target_properties(pose_common_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_ARCHITECTURES "${POSE_CUDA_ARCH}"
    )
    target_include_directories(pose_common_cuda PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(pose_common_cuda PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/blake3
    )
    target_link_libraries(pose_common_cuda PUBLIC pose_common)
endif()

# ── benchmark ─────────────────────────────────────────────────────────────────

add_executable(bench_region bench/bench_region.c)
target_link_libraries(bench_region PRIVATE pose_common)
target_include_directories(bench_region PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(BENCH_ALGO "blake3" CACHE STRING "Hash algorithm for bench_region: blake3, sha256, or aes_prf")
if(BENCH_ALGO STREQUAL "sha256")
    target_compile_definitions(bench_region PRIVATE BENCH_ALGO_SHA256=1)
elseif(BENCH_ALGO STREQUAL "aes_prf")
    target_compile_definitions(bench_region PRIVATE BENCH_ALGO_AES_PRF=1)
endif()

# ── test harness ──────────────────────────────────────────────────────────────
enable_testing()

add_executable(test_hash test/test_hash.c)
target_link_libraries(test_hash PRIVATE pose_common)
target_include_directories(test_hash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME hash_vectors COMMAND test_hash)

add_executable(test_graph test/test_graph.c)
target_link_libraries(test_graph PRIVATE pose_common)
target_include_directories(test_graph PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME graph_round_trip COMMAND test_graph)

add_executable(test_region test/test_region.c)
target_link_libraries(test_region PRIVATE pose_common)
target_include_directories(test_region PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME region_labeling COMMAND test_region)

add_executable(test_inplace test/test_inplace.c)
target_link_libraries(test_inplace PRIVATE pose_common)
target_include_directories(test_inplace PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME inplace_labeling COMMAND test_inplace)

add_executable(test_slot_map test/test_slot_map.c)
target_link_libraries(test_slot_map PRIVATE pose_common)
target_include_directories(test_slot_map PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
add_test(NAME slot_map COMMAND test_slot_map)
