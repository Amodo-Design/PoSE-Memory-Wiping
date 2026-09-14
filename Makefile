# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

# PoSE-Memory-Wiping — convenience targets.
#
# Everything here is a thin wrapper around CMake.  The CUDA targets run inside
# the container from ./Dockerfile so nvcc is not needed on the build machine;
# on a GPU host with nvcc installed you can run the same cmake commands
# directly (see tools/disk-wipe-bench/README.md).

DOCKER_IMAGE ?= pose-build-cuda
DOCKER       := docker run --rm --platform linux/amd64 -v $(CURDIR):/work -w /work $(DOCKER_IMAGE)
NPROC        := $(shell nproc 2>/dev/null || sysctl -n hw.logicalcpu)

# GPU labeling strategy: level (byte-parity reference) | block | thread | warp (fastest measured).
GPU_STRATEGY    ?= warp
# warp only: min resident blocks/SM for __launch_bounds__ (3, or 4 if ptxas reports zero spills).
WARP_MIN_BLOCKS ?= 3
# CUDA compute capability: 90 = Hopper (H100/H200), 100 = Blackwell (GB200).
CUDA_ARCH       ?= 90

.PHONY: help docker common-test disk-wipe-bench disk-wipe-bench-clean clean

help:
	@echo "  make common-test              build the CPU labeling library natively and run its tests"
	@echo "  make docker                   build the CUDA build container ($(DOCKER_IMAGE))"
	@echo "  make disk-wipe-bench          build tools/disk-wipe-bench in the container"
	@echo "                                [GPU_STRATEGY=level|block|thread|warp] [WARP_MIN_BLOCKS=3|4] [CUDA_ARCH=90|100]"
	@echo "  make disk-wipe-bench-clean    remove the disk-wipe-bench build tree (required when switching GPU_STRATEGY)"
	@echo "  make clean                    remove all build trees"

# CPU library + unit tests, native (no CUDA needed).
common-test:
	cmake -S common -B common/build -DCMAKE_BUILD_TYPE=Release
	cmake --build common/build -j$(NPROC)
	ctest --test-dir common/build --output-on-failure

docker:
	docker build --platform linux/amd64 -t $(DOCKER_IMAGE) .

# GPU-driven disk wipe + throughput benchmark.  The strategy is cached by CMake:
# run `make disk-wipe-bench-clean` before switching GPU_STRATEGY.
disk-wipe-bench:
	$(DOCKER) cmake -S tools/disk-wipe-bench -B tools/disk-wipe-bench/build \
		-DCMAKE_BUILD_TYPE=Release -DPOSE_CUDA_ARCH=$(CUDA_ARCH) \
		-DPOSE_GPU_LABEL_STRATEGY=$(GPU_STRATEGY) -DPOSE_GPU_WARP_MIN_BLOCKS=$(WARP_MIN_BLOCKS)
	$(DOCKER) cmake --build tools/disk-wipe-bench/build -j$(NPROC)
	@echo "built: tools/disk-wipe-bench/build/disk-wipe-bench (run on the GPU host as root; see tools/disk-wipe-bench/README.md)"

disk-wipe-bench-clean:
	rm -rf tools/disk-wipe-bench/build

clean: disk-wipe-bench-clean
	rm -rf common/build common/build-*
