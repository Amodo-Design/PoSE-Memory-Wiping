# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

# CMake toolchain for cross-compiling for ASpeed 32-bit ARMv7 BMC parts
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR armv7l)

set(CMAKE_C_COMPILER   arm-linux-gnueabihf-gcc)
set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
set(CMAKE_ASM_COMPILER arm-linux-gnueabihf-gcc)

# -mfpu=vfpv4-d16 is deliberate and must not be dropped: -mcpu=cortex-a7 alone
# defaults the FPU to neon-vfpv4, which would emit NEON instructions this core
# does not implement.  d16 matches the vfpv3d16 in the device's cpuinfo.
set(POSE_ARMHF_ISA_FLAGS "-mcpu=cortex-a7 -mfpu=vfpv4-d16 -mfloat-abi=hard")
set(CMAKE_C_FLAGS_INIT   "${POSE_ARMHF_ISA_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${POSE_ARMHF_ISA_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${POSE_ARMHF_ISA_FLAGS}")

# CMAKE_LIBRARY_ARCHITECTURE steers find_library at the armhf multiarch directory
# so it cannot latch onto the host's arm64 libraries
set(CMAKE_LIBRARY_ARCHITECTURE arm-linux-gnueabihf)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
