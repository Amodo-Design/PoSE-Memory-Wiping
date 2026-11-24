# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# Build image for everything in this repo: the Ubuntu 24.04 (noble) kernels and
# their out-of-tree modules, the daemons for all three architectures, and the
# CUDA toolkit for the source-daemon's GPU labelling (x86_64 host with NVIDIA
# GPUs).  One image, so a build never depends on which one was made.
#
# The image is built FROM the Ubuntu release the kernel will run on, so the
# toolchain (gcc-13 on noble) matches what Canonical used.  That matters for
# out-of-tree modules against a *stock* Ubuntu kernel: a different gcc major
# changes the vermagic/ABI assumptions and modprobe refuses the module.
#
# It is arch-neutral.  `ubuntu:24.04` is a multi-arch image, so the native
# toolchain is whatever the build host is (amd64 or arm64), and the cross
# toolchain for the *other* architecture is added on top.  Both targets can
# therefore be built from one container on either kind of host with no
# qemu emulation.
#
# Build the image (from the repo root; either name works on podman or docker):
#   make docker                       (= docker build -t pose-build docker)
#   docker build -t pose-build --build-arg BASE=ubuntu:26.04 --build-arg CUDA_VER=13-5 docker
#
# Run it with the kernel tree bind-mounted at /work:
#   docker run --rm -it -v "$PWD":/work pose-build
#
# Inside — upstream-style build, native (host arch == target arch):
#   make olddefconfig && make -j"$(nproc)" bindeb-pkg
#
# Inside — cross build (host != target); pick the matching pair:
#   make ARCH=arm64  CROSS_COMPILE=aarch64-linux-gnu- -j"$(nproc)" Image modules bindeb-pkg
#   make ARCH=x86_64 CROSS_COMPILE=x86_64-linux-gnu-  -j"$(nproc)" bzImage modules bindeb-pkg
#
# Inside — out-of-tree module against a stock kernel's headers (unpack the target's
# linux-headers-<ver> .deb under /work or mount /usr/src from the target box):
#   make -C /work/usr/src/linux-headers-6.8.0-xx-generic M="$PWD" modules \
#        [ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-]

ARG BASE=ubuntu:24.04
FROM ${BASE}

ARG DEBIAN_FRONTEND=noninteractive

# Toolchain and kernel build requirements
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    bc \
    bison \
    bzip2 \
    flex \
    libssl-dev \
    libelf-dev \
    libdw-dev \
    dwarves \
    kmod \
    cpio \
    zstd \
    xz-utils \
    lz4 \
    gzip \
    gawk \
    perl \
    python3 \
    ccache \
    git \
    wget \
    curl \
    ca-certificates \
    cmake \
    && rm -rf /var/lib/apt/lists/*

# Cross toolchain for armv7.
RUN apt-get update && apt-get install -y --no-install-recommends \
    crossbuild-essential-armhf \
    && rm -rf /var/lib/apt/lists/*

# Cross toolchain for whichever of x86_64 / aarch64 is NOT the build host.
RUN set -eux; \
    apt-get update; \
    case "$(dpkg --print-architecture)" in \
        amd64) apt-get install -y --no-install-recommends crossbuild-essential-arm64 ;; \
        arm64) apt-get install -y --no-install-recommends crossbuild-essential-amd64 ;; \
        *)     echo "unsupported build host architecture: $(dpkg --print-architecture)" >&2; exit 1 ;; \
    esac; \
    rm -rf /var/lib/apt/lists/*

# ── CUDA toolkit ─────────────────────────────────────────────────────────────
# The toolkit matches the build host: on an amd64 host nvcc is the ordinary
# x86_64 one; on an arm64 host it is the sbsa one, and the x86_64 *target*
# headers and libraries (cudart, cccl, crt, culibos, driver stub) are unpacked
# from the amd64 packages into /usr/local/cuda/targets/x86_64-linux/, so nvcc
# cross-compiles with "-ccbin x86_64-linux-gnu-g++ -target-dir x86_64-linux"
# (device code is host-independent; NVIDIA only documents the x86 -> arm
# direction, this is that recipe mirrored).

ARG CUDA_VER=13-4
ARG CUDA_REPO=https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2404

RUN set -eux; \
    case "$(dpkg --print-architecture)" in amd64) repo=x86_64 ;; arm64) repo=sbsa ;; *) exit 1 ;; esac; \
    curl -fsSL -o /tmp/cuda-keyring.deb "$CUDA_REPO/$repo/cuda-keyring_1.1-1_all.deb"; \
    dpkg -i /tmp/cuda-keyring.deb; rm /tmp/cuda-keyring.deb; \
    apt-get update; \
    apt-get install -y --no-install-recommends "cuda-nvcc-$CUDA_VER" "cuda-cudart-dev-$CUDA_VER"; \
    rm -rf /var/lib/apt/lists/*

# On an arm64 host, add the x86_64 target tree from the amd64 packages (the
# newest version of each in the repository index).  cuda-nvcc is in the list
# only for the headers it puts under targets/ (fatbinary_section.h); its x86
# binaries are not copied.
RUN set -eux; \
    [ "$(dpkg --print-architecture)" = arm64 ] || exit 0; \
    cuda_dir="/usr/local/cuda-$(echo "$CUDA_VER" | tr - .)"; \
    mkdir -p /tmp/x86/root; cd /tmp/x86; \
    curl -fsSL -o Packages "$CUDA_REPO/x86_64/Packages"; \
    for p in "cuda-cudart-$CUDA_VER" "cuda-cudart-dev-$CUDA_VER" "cccl-$CUDA_VER" "cuda-crt-$CUDA_VER" \
             "cuda-culibos-dev-$CUDA_VER" "cuda-driver-dev-$CUDA_VER" "cuda-nvcc-$CUDA_VER"; do \
        f=$(awk -v p="Package: $p" '$0 == p { f = 1 } f && /^Version:/ { v = $2 } f && /^Filename:/ { print v, $2; f = 0 }' Packages \
            | sort -V | tail -1 | cut -d' ' -f2); \
        [ -n "$f" ] || { echo "no amd64 package $p in the CUDA repository" >&2; exit 1; }; \
        curl -fsSL -o pkg.deb "$CUDA_REPO/x86_64/$f"; \
        dpkg-deb -x pkg.deb root; \
    done; \
    cp -a "root$cuda_dir/targets/x86_64-linux" "$cuda_dir/targets/"; \
    cd /; rm -rf /tmp/x86

# And the mirror image: on an amd64 host, add the sbsa (arm64 server) target
# tree from the sbsa packages, so the aarch64 source-daemon (a Grace host)
# cross-compiles with "-ccbin aarch64-linux-gnu-g++ -target-dir sbsa-linux".
RUN set -eux; \
    [ "$(dpkg --print-architecture)" = amd64 ] || exit 0; \
    cuda_dir="/usr/local/cuda-$(echo "$CUDA_VER" | tr - .)"; \
    mkdir -p /tmp/sbsa/root; cd /tmp/sbsa; \
    curl -fsSL -o Packages "$CUDA_REPO/sbsa/Packages"; \
    for p in "cuda-cudart-$CUDA_VER" "cuda-cudart-dev-$CUDA_VER" "cccl-$CUDA_VER" "cuda-crt-$CUDA_VER" \
             "cuda-culibos-dev-$CUDA_VER" "cuda-driver-dev-$CUDA_VER" "cuda-nvcc-$CUDA_VER"; do \
        f=$(awk -v p="Package: $p" '$0 == p { f = 1 } f && /^Version:/ { v = $2 } f && /^Filename:/ { print v, $2; f = 0 }' Packages \
            | sort -V | tail -1 | cut -d' ' -f2); \
        [ -n "$f" ] || { echo "no sbsa package $p in the CUDA repository" >&2; exit 1; }; \
        curl -fsSL -o pkg.deb "$CUDA_REPO/sbsa/$f"; \
        dpkg-deb -x pkg.deb root; \
    done; \
    cp -a "root$cuda_dir/targets/sbsa-linux" "$cuda_dir/targets/"; \
    cd /; rm -rf /tmp/sbsa

ENV PATH=/usr/local/cuda/bin:$PATH

# Keep the kernel's own vermagic/compiler strings deterministic across hosts.
ENV KBUILD_BUILD_USER=pose KBUILD_BUILD_HOST=pose-kernel-build

WORKDIR /work
CMD ["/bin/bash"]
