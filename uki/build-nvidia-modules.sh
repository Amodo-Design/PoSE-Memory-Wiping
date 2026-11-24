#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-nvidia-modules.sh — build the NVIDIA GPU kernel modules (compute only)
# against a kernel from uki/build-kernel.sh, so the x86 host UKI can run CUDA to
# wipe GPU HBM.
#
# Inputs:
#   vendor/kernel-src/<kver>/           kernel source (vendor/fetch-ubuntu-kernel-src.sh)
#   vendor/kernel-build/<kver>/obj/     kernel object tree (uki/build-kernel.sh)
#   vendor/nvidia-src/nvidia-<ver>/     the nvidia-kernel-source tree
#                                       (vendor/fetch-nvidia-modules-src.sh)
# Output:
#   vendor/kernel-build/<kver>/modules/<kver>/updates/dkms/{nvidia,nvidia-uvm}.ko
#       stripped, signed, in Ubuntu's updates/dkms layout.
#
# Usage: uki/build-nvidia-modules.sh [options] KVER
#   -o DIR     nvidia source dir  (default: newest vendor/nvidia-src/nvidia-*)
#   -h         help
#
# The in-container build is uki/build-nvidia-container.sh.
#
# Needs: docker or podman.  Nothing is run on the targets.

set -euo pipefail

NVSRC=""

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "o:h" opt; do
    case "$opt" in
        o) NVSRC=$OPTARG ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one kernel version is required, e.g. 6.8.0-147-generic (see -h)" >&2; exit 1; }
KVER=$1
case "$KVER" in *bluefield*) DEBARCH=arm64 ;; *) DEBARCH=amd64 ;; esac

cd "$(dirname "$(readlink -f "$0")")/.."
REPO=$PWD
SRC="vendor/kernel-src/$KVER"
KB="vendor/kernel-build/$KVER"
if [[ -z $NVSRC ]]; then
    NVSRC=$(ls -d vendor/nvidia-src/nvidia-* 2>/dev/null | sort -V | tail -n 1 || true)
fi
[[ -f $SRC/Makefile ]]          || { echo "error: $SRC missing; run vendor/fetch-ubuntu-kernel-src.sh $KVER" >&2; exit 1; }
[[ -f $KB/obj/Module.symvers ]] || { echo "error: $KB/obj has no Module.symvers; run uki/build-kernel.sh $KVER" >&2; exit 1; }
[[ -n $NVSRC && -f $NVSRC/Makefile ]] || { echo "error: no nvidia source under vendor/nvidia-src; run vendor/fetch-nvidia-modules-src.sh <version> (or -o DIR)" >&2; exit 1; }
NVSRC=${NVSRC#"$REPO"/}
NVVER=$(basename "$NVSRC"); NVVER=${NVVER#nvidia-}

exec docker run --rm -v "$REPO:/work" \
    -e SRC="/work/$SRC" -e OUT="/work/$KB" -e PRISTINE="/work/$NVSRC" \
    -e KVER="$KVER" -e NVVER="$NVVER" -e DEBARCH="$DEBARCH" \
    pose-build bash /work/uki/build-nvidia-container.sh
