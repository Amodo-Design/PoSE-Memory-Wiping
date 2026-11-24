#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-ofed-modules.sh — build the NVIDIA OFED (DOCA) mlx5 module closure
# against a kernel built by uki/build-kernel.sh, so the UKI can drive the
# BlueField ConnectX eswitch.
#
# Inputs:
#   vendor/kernel-src/<kver>/           kernel source (vendor/fetch-ubuntu-kernel-src.sh)
#   vendor/kernel-build/<kver>/obj/     kernel object tree (uki/build-kernel.sh)
#   vendor/ofed-src/usr/src/mlnx-ofed-kernel-<ver>/
#       unpacked mlnx-ofed-kernel-dkms_<ver>_all.deb (arch-independent) from the
#       DOCA repo (vendor/fetch-ofed-modules-src.sh).  The version must match the DOCA
#       release the DPU runs (2607.0.100-1 for 3.5.0).
# Output:
#   vendor/kernel-build/<kver>/modules/<kver>/updates/dkms/{mlx_compat,mlxfw,mlxdevm,mlx5_core}.ko
#       stripped, Ubuntu's updates/dkms layout; the in-tree modules sit under kernel/
#   vendor/kernel-build/<kver>/ofed/     the OFED build tree (configure writes into
#       its source tree, so the pristine unpack is copied here first)
#
# Usage: uki/build-ofed-modules.sh [options] KVER
#   -o DIR     OFED source dir   (default: newest vendor/ofed-src/usr/src/mlnx-ofed-kernel-*)
#   -n         configure only
#   -h         help
#
# The in-container build is uki/build-ofed-container.sh.
#
# Needs: docker or podman.  Nothing is run on the targets.

set -euo pipefail

OFEDSRC=""
CONFIG_ONLY=0

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "o:nh" opt; do
    case "$opt" in
        o) OFEDSRC=$OPTARG ;;
        n) CONFIG_ONLY=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one kernel version is required, e.g. 6.8.0-1030-bluefield (see -h)" >&2; exit 1; }
KVER=$1
case "$KVER" in *bluefield*) DEBARCH=arm64 ;; *) DEBARCH=amd64 ;; esac

cd "$(dirname "$(readlink -f "$0")")/.."
REPO=$PWD
SRC="vendor/kernel-src/$KVER"
KB="vendor/kernel-build/$KVER"
if [[ -z $OFEDSRC ]]; then
    OFEDSRC=$(ls -d vendor/ofed-src/usr/src/mlnx-ofed-kernel-* 2>/dev/null | sort -V | tail -n 1 || true)
fi
[[ -f $SRC/Makefile ]]        || { echo "error: $SRC missing; run vendor/fetch-ubuntu-kernel-src.sh $KVER" >&2; exit 1; }
[[ -f $KB/obj/Module.symvers ]] || { echo "error: $KB/obj has no Module.symvers; run uki/build-kernel.sh $KVER" >&2; exit 1; }
[[ -n $OFEDSRC && -f $OFEDSRC/configure ]] || { echo "error: no OFED source under vendor/ofed-src; run vendor/fetch-ofed-modules-src.sh <DOCA release> (or -o DIR)" >&2; exit 1; }
OFEDSRC=${OFEDSRC#"$REPO"/}
OFEDVER=$(basename "$OFEDSRC"); OFEDVER=${OFEDVER#mlnx-ofed-kernel-}

exec docker run --rm -v "$REPO:/work" \
    -e SRC="/work/$SRC" -e OUT="/work/$KB" -e PRISTINE="/work/$OFEDSRC" \
    -e KVER="$KVER" -e OFEDVER="$OFEDVER" -e DEBARCH="$DEBARCH" \
    -e CONFIG_ONLY="$CONFIG_ONLY" \
    pose-build bash /work/uki/build-ofed-container.sh
