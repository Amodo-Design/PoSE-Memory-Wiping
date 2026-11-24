# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# Everything builds inside the container from docker/Dockerfile, with the repo
# bind-mounted at /work.  Each target architecture has a toolchain file in
# cmake/; on Ubuntu the native compiler also answers to its triplet name, so the
# same toolchain file works whether the build host is arm64 or amd64.

IMAGE  := pose-build
DOCKER := docker run --rm -v $(CURDIR):/work -w /work $(IMAGE)

# Toolchain file (cmake/<triplet>.cmake) for each target architecture.
TOOLCHAIN_aarch64 := aarch64-linux-gnu
TOOLCHAIN_x86_64  := x86_64-linux-gnu
TOOLCHAIN_armv7   := armv7-linux-gnueabihf

# Ubuntu kernel each UKI is built from, per architecture (the DPU's bluefield
# kernel and the x86 host's generic one)
KVER_aarch64 := 6.8.0-1030-bluefield
KVER_x86_64  := 6.8.0-147-generic

# cmake configure + build of one project for one architecture:
#   $(call cmake_build,<source dir>,<arch>)  ->  <source dir>/build-<arch>/
cmake_build = $(DOCKER) bash -c " \
	  cmake -S /work/$(1) -B /work/$(1)/build-$(2) \
	    -DCMAKE_TOOLCHAIN_FILE=/work/cmake/$(TOOLCHAIN_$(2)).cmake \
	    -DCMAKE_BUILD_TYPE=MinSizeRel $(3) && \
	  cmake --build /work/$(1)/build-$(2)"

.PHONY: help docker kernel-aarch64 kernel-x86_64 \
	pose-common-aarch64 pose-common-x86_64 pose-common-test pose-common-clean \
	pose-init-aarch64 pose-init-x86_64 pose-init-clean \
	sink-daemon-aarch64 sink-daemon-x86_64 sink-daemon-armv7 sink-daemon-clean \
	source-daemon-aarch64 source-daemon-x86_64 source-daemon-clean \
	verifier verifier-test verifier-clean \
	disk-wipe-bench disk-wipe-bench-clean

# disk-wipe-bench only.  GPU labeling strategy: level (byte-parity reference) |
# block | thread | warp (fastest measured).
GPU_STRATEGY    ?= warp
# warp only: min resident blocks/SM for __launch_bounds__ (3, or 4 if ptxas reports zero spills).
WARP_MIN_BLOCKS ?= 3
# CUDA compute capability: 90 = Hopper (H100/H200), 100 = Blackwell (GB200).
CUDA_ARCH       ?= 90

help:
	@echo "Targets:"
	@echo "  make docker              build the $(IMAGE) image (docker/Dockerfile)"
	@echo "  make kernel-aarch64      DPU kernel $(KVER_aarch64)  -> vendor/kernel-build/$(KVER_aarch64)/"
	@echo "  make kernel-x86_64       host kernel $(KVER_x86_64)    -> vendor/kernel-build/$(KVER_x86_64)/"
	@echo "  make pose-common-aarch64 labeling library, tests, pose-label -> pose-common/build-aarch64/"
	@echo "  make pose-common-x86_64  labeling library, tests, pose-label -> pose-common/build-x86_64/"
	@echo "  make pose-common-test    build for the container's own arch and run the tests"
	@echo "  make pose-common-clean   remove the pose-common build directories"
	@echo "  make pose-init-aarch64   static PID 1 shim -> pose-init/build-aarch64/pose-init"
	@echo "  make pose-init-x86_64    static PID 1 shim -> pose-init/build-x86_64/pose-init"
	@echo "  make pose-init-clean     remove the pose-init build directories"
	@echo "  make sink-daemon-aarch64 label sink (DPU)       -> sink-daemon/build-aarch64/sink-daemon"
	@echo "  make sink-daemon-x86_64  label sink (x86 host)  -> sink-daemon/build-x86_64/sink-daemon"
	@echo "  make sink-daemon-armv7   label sink (BMC)       -> sink-daemon/build-armv7/sink-daemon"
	@echo "  make sink-daemon-clean   remove the sink-daemon build directories"
	@echo "  make source-daemon-x86_64  tray host daemon (x86) -> source-daemon/build-x86_64/source-daemon"
	@echo "  make source-daemon-aarch64 tray host daemon (Grace, testing) -> source-daemon/build-aarch64/source-daemon"
	@echo "  make source-daemon-clean   remove the source-daemon build directories"
	@echo "  make verifier            pose-verifier, natively (cargo) -> pose-verifier/target/release/pose-verifier"
	@echo "  make verifier-test       run the verifier's tests (codec vs C encoders, labels vs pose-common)"
	@echo "  make verifier-clean      remove pose-verifier/target"
	@echo "  make disk-wipe-bench     GPU disk wipe benchmark (x86 host) -> tools/disk-wipe-bench/build/disk-wipe-bench"
	@echo "                           [GPU_STRATEGY=level|block|thread|warp] [WARP_MIN_BLOCKS=3|4] [CUDA_ARCH=90|100]"
	@echo "  make disk-wipe-bench-clean remove the disk-wipe-bench build directory (required when switching GPU_STRATEGY)"

# Cheap when nothing changed: every layer comes from the cache.
docker:
	docker build -t $(IMAGE) docker

# Ubuntu's config for the flavour plus uki/kconfig-fragments/, built in the
# container; needs the source from vendor/fetch-ubuntu-kernel-src.sh first.
kernel-aarch64: docker
	uki/build-kernel.sh $(KVER_aarch64)

kernel-x86_64: docker
	uki/build-kernel.sh $(KVER_x86_64)

# Shared labeling library (graph + hash + CUDA GPU labeling), its tests and the
# pose-label CLI.  The daemons pull in only the library, via add_subdirectory.
# TARGET_GPU=ON here because the image always has nvcc; the CUDA parts build
# for sm_90/100/103 (H200, GB200, GB300) on either architecture.  The gpu_parity
# test only runs where there is a GPU (it skips elsewhere).
pose-common-aarch64: docker
	$(call cmake_build,pose-common,aarch64,-DTARGET_GPU=ON)

pose-common-x86_64: docker
	$(call cmake_build,pose-common,x86_64,-DTARGET_GPU=ON)

# Tests run where they can execute: the container's own architecture.
pose-common-test: docker
	$(DOCKER) bash -c ' \
	  arch=$$(uname -m); \
	  cmake -S /work/pose-common -B /work/pose-common/build-$$arch \
	    -DCMAKE_TOOLCHAIN_FILE=/work/cmake/$$arch-linux-gnu.cmake \
	    -DCMAKE_BUILD_TYPE=MinSizeRel && \
	  cmake --build /work/pose-common/build-$$arch && \
	  ctest --test-dir /work/pose-common/build-$$arch --output-on-failure'

pose-common-clean:
	rm -rf pose-common/build-aarch64 pose-common/build-x86_64 pose-common/build-host

pose-init-aarch64: docker
	$(call cmake_build,pose-init,aarch64)

pose-init-x86_64: docker
	$(call cmake_build,pose-init,x86_64)

pose-init-clean:
	rm -rf pose-init/build-aarch64 pose-init/build-x86_64

sink-daemon-aarch64: docker
	$(call cmake_build,sink-daemon,aarch64)

sink-daemon-x86_64: docker
	$(call cmake_build,sink-daemon,x86_64)

sink-daemon-armv7: docker
	$(call cmake_build,sink-daemon,armv7)

sink-daemon-clean:
	rm -rf sink-daemon/build-aarch64 sink-daemon/build-x86_64 sink-daemon/build-armv7

# GPU targets are on by default in source-daemon's CMakeLists, but the flag is
# passed explicitly so a build directory configured before GPU support (whose
# cache says OFF) cannot silently yield a daemon without it.
source-daemon-aarch64: docker
	$(call cmake_build,source-daemon,aarch64,-DTARGET_GPU=ON)

source-daemon-x86_64: docker
	$(call cmake_build,source-daemon,x86_64,-DTARGET_GPU=ON)

source-daemon-clean:
	rm -rf source-daemon/build-aarch64 source-daemon/build-x86_64

# The verifier runs on the operator's machine, so it is built natively, not
# in the container; build.rs builds pose-common through its CMakeLists.txt.
verifier:
	cd pose-verifier && cargo build --release

verifier-test:
	cd pose-verifier && cargo test --release

verifier-clean:
	rm -rf pose-verifier/target

# GPU-driven disk wipe + throughput benchmark, for the x86_64 GPU host.  The
# strategy is cached by CMake: run `make disk-wipe-bench-clean` before
# switching GPU_STRATEGY.
disk-wipe-bench: docker
	$(DOCKER) bash -c " \
	  cmake -S /work/tools/disk-wipe-bench -B /work/tools/disk-wipe-bench/build \
	    -DCMAKE_TOOLCHAIN_FILE=/work/cmake/$(TOOLCHAIN_x86_64).cmake \
	    -DCMAKE_BUILD_TYPE=Release -DPOSE_CUDA_ARCH=$(CUDA_ARCH) \
	    -DPOSE_GPU_LABEL_STRATEGY=$(GPU_STRATEGY) -DPOSE_GPU_WARP_MIN_BLOCKS=$(WARP_MIN_BLOCKS) && \
	  cmake --build /work/tools/disk-wipe-bench/build"
	@echo "built: tools/disk-wipe-bench/build/disk-wipe-bench (run on the GPU host as root; see tools/disk-wipe-bench/README.md)"

disk-wipe-bench-clean:
	rm -rf tools/disk-wipe-bench/build
