# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

cmake_minimum_required(VERSION 3.16)
project(pose_sink C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Everything that makes a machine a label sink, shared by sink-daemon and
# source-daemon: the wire protocol, the TCP transport (both ends), this
# machine's own targets (carved-out RAM, whole disks), and the self-wipe exit
# path.  No graph or hash code, no TLS.

set(POSE_SINK_SOURCES
    src/sink_proto.c
    src/sink_net.c
    src/sink_inventory.c
    src/sink_session.c
    src/sink_cache.c
    src/self_wipe.c
)

# The self-wipe trampoline is architecture-specific assembly; armv7 takes the
# C fallback in self_wipe.c.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    list(APPEND POSE_SINK_SOURCES src/self_wipe_aarch64.S)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    list(APPEND POSE_SINK_SOURCES src/self_wipe_x86_64.S)
endif()

add_library(pose-sink STATIC ${POSE_SINK_SOURCES})
target_include_directories(pose-sink PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(pose-sink PRIVATE -Wall -Wextra -Wpedantic -Werror)
# off_t must be 64-bit everywhere, armv7 included: /dev/mem mmap offsets are
# physical addresses, and disk offsets pass 4 GiB.
target_compile_definitions(pose-sink PUBLIC _FILE_OFFSET_BITS=64)
target_compile_definitions(pose-sink PRIVATE _GNU_SOURCE)
