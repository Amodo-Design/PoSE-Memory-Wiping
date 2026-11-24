# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

cmake_minimum_required(VERSION 3.16)
project(sink_daemon C ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../pose-sink pose-sink)

add_executable(sink-daemon src/sink_main.c)
target_link_libraries(sink-daemon PRIVATE pose-sink)
target_compile_options(sink-daemon PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(sink-daemon PRIVATE _GNU_SOURCE)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/pose-runtime.cmake)
pose_daemon_link(sink-daemon)
