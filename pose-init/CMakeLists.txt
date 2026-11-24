# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

cmake_minimum_required(VERSION 3.16)
project(pose_init C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(pose-init pose_init.c)
target_compile_options(pose-init PRIVATE -Wall -Wextra -Wpedantic -Werror -Os)

target_link_options(pose-init PRIVATE -static)
