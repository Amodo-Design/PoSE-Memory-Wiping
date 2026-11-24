#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# fetch-nvidia-modules-src.sh — fetch the NVIDIA open kernel module source, version
# VERSION, that uki/build-nvidia-modules.sh builds nvidia.ko and nvidia-uvm.ko
# from.
#
# It is the /usr/src/nvidia-<ver> tree of Ubuntu's nvidia-kernel-source-<N>-open
# package, the one nvidia-dkms-<N>-open builds from on a normal host.  The tree
# is not architecture-independent: it carries precompiled objects
# (nv-kernel.o_binary), so fetch it for the architecture of the machine the
# modules will run on.
#
# VERSION must be the version given to vendor/fetch-nvidia.sh: the kernel
# modules, the GSP firmware and libcuda only work together at one exact version.
#
# Output:
#   vendor/nvidia-src/nvidia-<ver>/            the source tree
#   vendor/nvidia-src/nvidia-<ver>/.source     the package it came from
#
# Usage: vendor/fetch-nvidia-modules-src.sh [-s SERIES] [-a ARCH] VERSION
#   VERSION     driver version, e.g. 595.99.02
#   -s SERIES   Ubuntu series to take the package from  (default noble, i.e. 24.04)
#   -a ARCH     Debian architecture of the target, amd64 or arm64  (default amd64)
#   -h          help
#
# Needs: curl, dpkg-deb, network access to archive.ubuntu.com or ports.ubuntu.com.

set -euo pipefail

SERIES=noble
ARCH=amd64
OUTDIR=vendor/nvidia-src
# amd64 lives on the main archive, every other architecture on ports
mirror_for() { case "$1" in amd64|i386) echo http://archive.ubuntu.com/ubuntu ;; *) echo http://ports.ubuntu.com/ubuntu-ports ;; esac; }

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "s:a:h" opt; do
    case "$opt" in
        s) SERIES=$OPTARG ;;
        a) ARCH=$OPTARG ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one driver version is required, e.g. 595.99.02 (see -h)" >&2; exit 1; }
VER=$1
[[ $VER =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "error: '$VER' is not a driver version (want e.g. 595.99.02)" >&2; exit 1; }
BRANCH=${VER%%.*}                   # 595
[[ $ARCH == amd64 || $ARCH == arm64 ]] || { echo "error: -a $ARCH: only amd64 and arm64 are supported" >&2; exit 1; }
for t in curl dpkg-deb; do
    command -v "$t" >/dev/null || { echo "error: $t is not installed" >&2; exit 1; }
done

cd "$(dirname "$(readlink -f "$0")")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# That exact version of the package, looking in -updates and -security first:
# driver releases land there after the series is released.
PKG=nvidia-kernel-source-$BRANCH-open    # nvidia-kernel-source-595-open
MIRROR=$(mirror_for "$ARCH")
echo "== $PKG $VER ($ARCH) from Ubuntu $SERIES"
deb=""
for pocket in "$SERIES-updates" "$SERIES-security" "$SERIES"; do
    echo "   probing $pocket/restricted ($ARCH) ..."
    deb=$(curl -fsSL "$MIRROR/dists/$pocket/restricted/binary-$ARCH/Packages.gz" | zcat \
          | awk -v p="$PKG" -v v="$VER" \
                '$0=="Package: "p{f=1; ok=0} f&&/^Version:/{ok=index($2, v)==1} f&&ok&&!hit&&/^Filename:/{print $2; hit=1} f&&/^$/{f=0}') || true
    [[ -n $deb ]] && break
done
[[ -n $deb ]] || { echo "error: $PKG $VER not found for $ARCH in $SERIES (try -s with another series)" >&2; exit 1; }

echo "   downloading $(basename "$deb")"
curl -fsSL -o "$TMP/src.deb" "$MIRROR/$deb"
dpkg-deb -x "$TMP/src.deb" "$TMP/x"
SRCDIR=$TMP/x/usr/src/nvidia-$VER
[[ -f $SRCDIR/Makefile ]] || { echo "error: no usr/src/nvidia-$VER in $(basename "$deb")" >&2; exit 1; }

DEST=$OUTDIR/nvidia-$VER
rm -rf "$DEST"; mkdir -p "$OUTDIR"
cp -a "$SRCDIR" "$DEST"
echo "$(basename "$deb") $MIRROR/$deb" > "$DEST/.source"
echo "== done: $DEST ($(du -sh "$DEST" | cut -f1), $ARCH, from $(basename "$deb"))"
