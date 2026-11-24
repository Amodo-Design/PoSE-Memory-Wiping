# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

cmake_minimum_required(VERSION 3.16)
project(pose_common C)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ── Target architecture detection ───────────────────────────────────────────
# ARM64 (Pi 5 / Grace): NEON BLAKE3 + ARMv8 crypto extensions for sha256/aes_prf.
# x86_64 (EPYC / H200 experiment): BLAKE3 SIMD backends (runtime-dispatched via
# CPUID) + AES-NI.
# ARM32 (armv7l BMC — AST2600, dual Cortex-A7 rev 5, VFPv4-D16, NO NEON):
# portable BLAKE3 and portable SHA-256 only.  The ARMv8 `-march=armv8-a+{sha2,
# crypto}` flags are invalid on ARMv7, the target has no crypto extensions, and
# it has no NEON either blake3_neon.c is not built and sha256.c takes its portable
# C fallback. aes_prf.c compiles to its abort stub — algo = aes_prf is NOT available
# on this target; blake3 and sha256 are.
#
# All three architectures produce byte-identical labels: the BLAKE3 backends are
# implementations of one specification, which is what label parity between the
# daemon, the verifier, and the GPU depends on.  Losing NEON costs throughput
# (1-wide instead of 4-wide hash_many), not correctness.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(POSE_ARCH "x86")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|AARCH64|ARM64)$")
    set(POSE_ARCH "aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7|armv7l|armv7-a|arm)$")
    set(POSE_ARCH "arm32")
else()
    message(FATAL_ERROR "pose_common: unsupported processor '${CMAKE_SYSTEM_PROCESSOR}' (expected x86_64, aarch64, or armv7l)")
endif()
message(STATUS "pose_common target architecture: ${POSE_ARCH}")

# Vendor BLAKE3 as a separate object library so -Werror doesn't apply to it.
set(BLAKE3_SOURCES
    vendor/blake3/blake3.c
    vendor/blake3/blake3_dispatch.c
    vendor/blake3/blake3_portable.c
)
if(POSE_ARCH STREQUAL "x86")
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
elseif(POSE_ARCH STREQUAL "aarch64")
    list(APPEND BLAKE3_SOURCES vendor/blake3/blake3_neon.c)
endif()

add_library(blake3_vendor OBJECT ${BLAKE3_SOURCES})

target_include_directories(blake3_vendor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/blake3
)

if(POSE_ARCH STREQUAL "x86")
    # Each SIMD backend compiles with only its own ISA flag so the others stay
    # callable on CPUs without it (the dispatcher gates them at runtime).
    set_source_files_properties(vendor/blake3/blake3_sse2.c   PROPERTIES COMPILE_OPTIONS "-msse2")
    set_source_files_properties(vendor/blake3/blake3_sse41.c  PROPERTIES COMPILE_OPTIONS "-msse4.1")
    set_source_files_properties(vendor/blake3/blake3_avx2.c   PROPERTIES COMPILE_OPTIONS "-mavx2")
    set_source_files_properties(vendor/blake3/blake3_avx512.c PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
elseif(POSE_ARCH STREQUAL "aarch64")
    # ARM64: NEON path; the dispatcher's x86 SIMD branches are compiled out
    # (IS_X86 is false), so the x86 sources are neither built nor referenced.
    target_compile_definitions(blake3_vendor PRIVATE
        BLAKE3_USE_NEON=1
        BLAKE3_NO_AVX512=1
        BLAKE3_NO_AVX2=1
        BLAKE3_NO_SSE41=1
        BLAKE3_NO_SSE2=1
    )
else()
    # ARM32: portable only.  BLAKE3_USE_NEON=0 is explicit rather than relying on
    # blake3_impl.h's autodetect (which infers 0 from "not IS_AARCH64"), so an
    # accidental NEON build on a core without it is impossible.  MAX_SIMD_DEGREE
    # becomes 1 and blake3_hash_many falls through to the portable backend for
    # any batch size, so pose_label_node_many's 16-wide batches still work.
    target_compile_definitions(blake3_vendor PRIVATE
        BLAKE3_USE_NEON=0
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
    src/gpu_scratch.c
    $<TARGET_OBJECTS:blake3_vendor>
)

# sha256.c has a portable C fallback.  aes_prf.c has two hardware backends that
# produce byte-identical labels: ARM AES-CE (-march=armv8-a+crypto) and x86
# AES-NI (-maes).  Without either ISA flag it compiles its abort fallback.  The
# ARMv8 -march flags are only valid on ARM64.
if(POSE_ARCH STREQUAL "x86")
    # AES-NI is present on every x86-64 server CPU (EPYC 9005 included); the
    # aes_prf.c TU is only entered for algo = aes_prf sessions.
    set_source_files_properties(src/aes_prf.c PROPERTIES
        COMPILE_OPTIONS "-maes"
    )
elseif(POSE_ARCH STREQUAL "aarch64")
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

if(NOT POSE_ARCH STREQUAL "x86")
    target_compile_definitions(pose_common PRIVATE
        BLAKE3_NO_AVX512=1
        BLAKE3_NO_AVX2=1
        BLAKE3_NO_SSE41=1
        BLAKE3_NO_SSE2=1
    )
endif()

# hash.c includes blake3_impl.h, so keep its BLAKE3_USE_NEON in step with the
# blake3_vendor objects it calls into.  ARM64 and ARM32 both match the header's
# autodetect (1 for AArch64, 0 otherwise); stating ARM32's explicitly means a
# stale cache or an out-of-tree consumer cannot silently disagree with the
# vendor objects about which backend exists.
if(POSE_ARCH STREQUAL "arm32")
    target_compile_definitions(pose_common PRIVATE BLAKE3_USE_NEON=0)
endif()

target_compile_options(pose_common PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)

# Large-file support, PUBLIC so every consumer of pose_common inherits it.
#
# On a 32-bit target (the armv7l BMC) off_t defaults to 32 bits, which silently
# truncates any file offset past 2 GiB — disk.c's pwrite/pread sector offsets and
# the /dev/mem offsets in memory.c/gpu.c both exceed that routinely.  Defining it
# on the command line guarantees it is in effect before the first libc header of
# every TU, which a per-file #define cannot guarantee for its includers.
# No-op on 64-bit targets, where off_t is already 64-bit.
target_compile_definitions(pose_common PUBLIC _FILE_OFFSET_BITS=64)

target_link_libraries(pose_common PUBLIC Threads::Threads)

# CPU labeling thread cap override (blank = header default 64).  The thread
# count actually used is detected at runtime by pose_region_threads() — the
# CPUs the process may run on, clamped to this maximum — so the cap only sizes
# the fixed scratch[POSE_REGION_THREADS_MAX] arrays and bounds peak scratch on
# many-core hosts.  Applied as a PUBLIC compile definition so every consumer
# that links pose_common (the daemons, pose_common_cuda, the tests) sees the
# same array sizes as the worker pool.
set(POSE_REGION_THREADS_MAX "" CACHE STRING "CPU labeling thread cap (blank = header default 64)")
if(POSE_REGION_THREADS_MAX)
    target_compile_definitions(pose_common PUBLIC POSE_REGION_THREADS_MAX=${POSE_REGION_THREADS_MAX})
endif()
if(POSE_REGION_THREADS)
    message(FATAL_ERROR "POSE_REGION_THREADS is no longer supported; the thread count is detected at runtime. Use POSE_REGION_THREADS_MAX to cap it.")
endif()

# ── GPU (CUDA) labeling — x86/H200 experiment ───────────────────────────────
# TARGET_GPU=ON enables the CUDA HBM labeling library.  Default OFF so the
# CPU-only ARM and x86 builds never require nvcc.
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
    set(POSE_CUDA_ARCH "90;100;103" CACHE STRING "CUDA compute capabilities to compile for (H200, GB200, GB300)")
    # The default for every target, including C consumers that only link the
    # library (CMake still wants it set on them for the device link step).
    set(CMAKE_CUDA_ARCHITECTURES "${POSE_CUDA_ARCH}")
    # GPU labeling parallelization strategy (one strategy translation unit is
    # compiled in alongside the shared host driver label_gpu_common.cu):
    #   level  (default) — full-scaffold wave, level-parallel across super-chunks
    #   block            — one thread block per super-chunk, recycled slot pool
    #   thread           — one thread per super-chunk, recycled slot pool
    #   warp             — one warp per super-chunk, recycled slot pool, __syncwarp()
    #                      between levels (no block barrier; see label_gpu_warp.cu)
    # Selected from the Makefile via -DPOSE_GPU_LABEL_STRATEGY=<name>.
    set(POSE_GPU_LABEL_STRATEGY "warp" CACHE STRING "GPU labeling strategy: level, block, thread, warp")
    # warp only: minimum resident blocks/SM requested via __launch_bounds__.
    # 3 matches BLOCK's register-limited occupancy; 4 forces <= 64 regs/thread
    # (check `-Xptxas -v` for zero spills before relying on it).
    set(POSE_GPU_WARP_MIN_BLOCKS "3" CACHE STRING "warp strategy: min blocks/SM for __launch_bounds__")
    add_library(pose_common_cuda STATIC
        src/cuda/blake3_node.cu
        src/cuda/label_gpu_common.cu
        src/cuda/gpu_session.cu
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
    # CUDA_RESOLVE_DEVICE_SYMBOLS: the device link happens inside this library,
    # so a plain C consumer (the source-daemon, the parity test) links it like
    # any other static library and needs no CUDA language of its own.
    set_target_properties(pose_common_cuda PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON
        CUDA_RESOLVE_DEVICE_SYMBOLS ON
        POSITION_INDEPENDENT_CODE ON
        CUDA_ARCHITECTURES "${POSE_CUDA_ARCH}"
    )
    # Static runtime: the binary then needs only the driver's libcuda.so.1 at
    # run time, which cudart dlopen()s, so no toolkit on the target.
    find_package(CUDAToolkit REQUIRED)
    target_link_libraries(pose_common_cuda PUBLIC CUDA::cudart_static)
    target_include_directories(pose_common_cuda PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_include_directories(pose_common_cuda PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/vendor/blake3
    )
    target_link_libraries(pose_common_cuda PUBLIC pose_common)
    # cudart_static and the C++ in this library need the C++ runtime, which a
    # plain-C consumer (the daemons) does not otherwise link.  The archive is
    # named on purpose: gcc's -static-libstdc++ only covers the implicit
    # library the g++ driver adds, not an explicit -lstdc++, and a dynamic
    # libstdc++.so.6 would have to be carried into the daemon's initramfs.
    target_link_libraries(pose_common_cuda PUBLIC -l:libstdc++.a)
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

# GPU <-> CPU parity gate: needs a CUDA device, so it is built with the library
# but only runs where one exists (ctest skips it otherwise, exit code 77).
if(TARGET_GPU)
    add_executable(test_gpu_parity test/test_gpu_parity.c)
    target_link_libraries(test_gpu_parity PRIVATE pose_common_cuda)
    target_include_directories(test_gpu_parity PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME gpu_parity COMMAND test_gpu_parity)
    set_tests_properties(gpu_parity PROPERTIES SKIP_RETURN_CODE 77)
endif()
