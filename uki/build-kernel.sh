#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-kernel.sh — build a kernel fetched by vendor/fetch-ubuntu-kernel-src.sh into
# the pieces a UKI needs: an uncompressed kernel image plus a stripped
# tree of plain .ko modules.
#
# The build runs inside the docker/Dockerfile container (image pose-build,
# created with `make docker`) so the toolchain is Ubuntu 24.04's gcc-13, the same one
# Canonical used.  The .config is Ubuntu's own for the flavour, exported from
# the tree's debian.<x>/config/annotations exactly as debian/rules does it, and
# the make invocation mirrors Ubuntu's kmake (KERNELRELEASE, KBUILD_BUILD_VERSION,
# PKG_ABI).  The result keeps the stock `uname -r` and vermagic, so out-of-tree
# modules built for the stock kernel (e.g. the DOCA dkms mlx5 stack) still load.
#
# Customisation is by kconfig fragments merged on top of the Ubuntu config
# before olddefconfig.  Every *.config in uki/kconfig-fragments/ is applied, in
# name order, on every build; -c adds further fragments after those.  Every
# fragment line is checked against the final .config and a mismatch is reported
# (a dependency may have forced it back).
#
# KVER is A.B.C-ABI-flavour as in `uname -r`; the flavour picks the Ubuntu
# config and the target architecture (*bluefield* -> arm64, else amd64, -a to
# override).  Cross compilation is automatic when the container's architecture
# differs from the target's.
#
# Output: vendor/kernel-build/<kver>/
#   Image | bzImage        uncompressed kernel (what uki/build-uki.sh wraps)
#   System.map, config
#   modules/<release>/     stripped modules tree (what uki/build-uki-dpu.sh and
#                          uki/build-uki-x86-host.sh take their modules from)
#   obj/                   kbuild object dir (large; kept for incremental rebuilds)
#
# This script only parses arguments, ensures the image exists and starts the
# container; the build itself is uki/build-kernel-container.sh, driven by the
# environment variables set here.
#
# Usage: uki/build-kernel.sh [options] KVER
#   -c FRAG    extra kconfig fragment, applied after uki/kconfig-fragments/*.config (repeatable)
#   -a ARCH    target Debian arch: arm64 | amd64  (default from flavour)
#   -s DIR     source root                        (default vendor/kernel-src)
#   -o DIR     output root                        (default vendor/kernel-build)
#   -n         configure only: write obj/.config and stop
#   -h         help

set -euo pipefail

FRAGDIR=uki/kconfig-fragments     # every *.config here is always applied, in name order
FRAGS=()
DEBARCH=""
SRCROOT=vendor/kernel-src
OUTROOT=vendor/kernel-build
CONFIG_ONLY=0

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "c:a:s:o:nh" opt; do
    case "$opt" in
        c) FRAGS+=("$(readlink -f "$OPTARG")") ;;
        a) DEBARCH=$OPTARG ;;
        s) SRCROOT=$OPTARG ;;
        o) OUTROOT=$OPTARG ;;
        n) CONFIG_ONLY=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one kernel version is required, e.g. 6.8.0-1030-bluefield (see -h)" >&2; exit 1; }
KVER=$1

cd "$(dirname "$(readlink -f "$0")")/.."

REPO=$PWD
SRC="$SRCROOT/$KVER"
OUT="$OUTROOT/$KVER"
[[ -f $SRC/Makefile && -f $SRC/debian/debian.env ]] || {
    echo "error: $SRC is not an Ubuntu kernel tree; fetch it first: vendor/fetch-ubuntu-kernel-src.sh $KVER" >&2; exit 1; }
mkdir -p "$OUTROOT/$KVER" "$OUTROOT/.ccache"

# Build list of kconfig fragments from those in the FRAGDIR and those passed in
# on the command line
frag_list=""
shopt -s nullglob; dirfrags=("$REPO/$FRAGDIR"/*.config); shopt -u nullglob
FRAGS=("${dirfrags[@]}" "${FRAGS[@]}")
for f in "${FRAGS[@]}"; do
    [[ $f == "$REPO"/* ]] || { echo "error: fragment $f must live inside the repo (it is read from the container)" >&2; exit 1; }
    frag_list+="${frag_list:+ }/work/${f#"$REPO"/}"
done

# Run the build in docker container. Ctrl-C does not stop it on its own so we
# install a signal trap to catch the interupt and shutdown the container build
NAME="pose-kernel-build-$KVER"
stop_build() {
    echo; echo "== interrupted: stopping $NAME"
    # Attempt a graceful shutdown by sending SIGINT to all the container
    # processes and only forcibly kill the container if it didn't disappear
    # on its own after 20 seconds
    docker exec "$NAME" bash -c 'kill -INT -1' >/dev/null 2>&1 || true
    for _ in $(seq 20); do docker inspect "$NAME" >/dev/null 2>&1 || break; sleep 1; done
    docker kill "$NAME" >/dev/null 2>&1 || true
    wait; exit 130
}
docker run --rm --name "$NAME" -v "$REPO:/work" -v "$REPO/$OUTROOT/.ccache:/ccache" \
    -e SRC="/work/$SRC" -e OUT="/work/$OUT" -e KVER="$KVER" -e DEBARCH="$DEBARCH" -e FRAGS="$frag_list" \
    -e CONFIG_ONLY="$CONFIG_ONLY" \
    pose-build bash /work/uki/build-kernel-container.sh &
trap stop_build INT TERM
wait $!
