#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# fetch-nvidia.sh — fetch the two pieces of the NVIDIA driver, version VERSION,
# that a UKI with GPUs packs into its initramfs (uki/build-uki-x86-host.sh):
#
#   the GSP firmware    nvidia.ko request_firmware()s nvidia/<ver>/gsp_*.bin when
#                       it initialises a GPU.  Only gsp_ga10x.bin is kept: it
#                       covers Ampere and later, including the H200s.
#                       gsp_tu10x.bin (Turing, ~30 MB) is dropped; -A keeps every
#                       file in the package.  Not architecture-specific.
#   libcuda.so.1        the CUDA driver library the source-daemon's statically
#                       linked cudart dlopen()s; fetched for every -a ARCH
#                       (default amd64 and arm64: the x86 host and a Grace host).
#
# Both must be the exact version of the kernel module that loads them, so
# VERSION must match the nvidia source the modules were built from
# (vendor/nvidia-src/nvidia-<ver>, vendor/fetch-nvidia-modules-src.sh).
#
# Output:
#   vendor/nvidia-firmware/nvidia/<ver>/gsp_ga10x.bin
#   vendor/nvidia-firmware/nvidia/<ver>/.source     the package it came from
#   vendor/nvidia-libcuda/<arch>/libcuda.so.1       x86_64 or aarch64
#   vendor/nvidia-libcuda/<arch>/.source
#
# Usage: vendor/fetch-nvidia.sh [-s SERIES] [-a ARCH]... [-A] VERSION
#   VERSION     driver version, e.g. 595.99.02
#   -s SERIES   Ubuntu series to take the packages from  (default noble, i.e. 24.04)
#   -a ARCH     fetch libcuda for this Debian architecture, amd64 or arm64;
#               repeatable (default: both).  -a none fetches the firmware only.
#   -A          keep all firmware files, not just gsp_ga10x.bin
#   -h          help
#
# Needs: curl, dpkg-deb, network access to archive.ubuntu.com and ports.ubuntu.com.

set -euo pipefail

SERIES=noble
KEEP_ALL=0
ARCHES=()
OUTDIR=vendor/nvidia-firmware
LIBDIR=vendor/nvidia-libcuda
# amd64 lives on the main archive, every other architecture on ports
mirror_for() { case "$1" in amd64|i386) echo http://archive.ubuntu.com/ubuntu ;; *) echo http://ports.ubuntu.com/ubuntu-ports ;; esac; }

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "s:a:Ah" opt; do
    case "$opt" in
        s) SERIES=$OPTARG ;;
        a) ARCHES+=("$OPTARG") ;;
        A) KEEP_ALL=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one driver version is required, e.g. 595.99.02 (see -h)" >&2; exit 1; }
VER=$1
[[ $VER =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] || { echo "error: '$VER' is not a driver version (want e.g. 595.99.02)" >&2; exit 1; }
BRANCH=${VER%%.*}                   # 595
[[ ${#ARCHES[@]} -gt 0 ]] || ARCHES=(amd64 arm64)
[[ ${ARCHES[*]} == none ]] && ARCHES=()
for a in "${ARCHES[@]}"; do
    [[ $a == amd64 || $a == arm64 ]] || { echo "error: -a $a: only amd64 and arm64 are supported" >&2; exit 1; }
done
for t in curl dpkg-deb; do
    command -v "$t" >/dev/null || { echo "error: $t is not installed" >&2; exit 1; }
done

cd "$(dirname "$(readlink -f "$0")")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# find_deb PKG ARCH VERSION -> "MIRROR FILENAME" of that exact version of PKG,
# looking in -updates and -security first: driver releases land there after
# the series is released.
find_deb() {
    local pkg=$1 arch=$2 ver=$3 mirror deb
    mirror=$(mirror_for "$arch")
    for pocket in "$SERIES-updates" "$SERIES-security" "$SERIES"; do
        echo "   probing $pocket/restricted ($arch) ..." >&2
        deb=$(curl -fsSL "$mirror/dists/$pocket/restricted/binary-$arch/Packages.gz" | zcat \
              | awk -v p="$pkg" -v v="$ver" \
                    '$0=="Package: "p{f=1; ok=0} f&&/^Version:/{ok=index($2, v)==1} f&&ok&&!hit&&/^Filename:/{print $2; hit=1} f&&/^$/{f=0}') || true
        [[ -n $deb ]] && { echo "$mirror $deb"; return 0; }
    done
    return 1
}
fetch_deb() {                        # fetch_deb "MIRROR FILENAME" OUT.deb
    echo "   downloading $(basename "${1#* }")"
    curl -fsSL -o "$2" "${1%% *}/${1#* }"
}

# ── firmware (not architecture-specific: take the amd64 package) ──────────────
PKG=nvidia-firmware-$BRANCH-$VER    # nvidia-firmware-595-595.99.02
echo "== $PKG from Ubuntu $SERIES"
src=$(find_deb "$PKG" amd64 "$VER") || { echo "error: $PKG not found in $SERIES (try -s with another series)" >&2; exit 1; }
deb=${src#* }
fetch_deb "$src" "$TMP/fw.deb"
dpkg-deb -x "$TMP/fw.deb" "$TMP/x"
SRCDIR=$TMP/x/lib/firmware/nvidia/$VER
[[ -d $SRCDIR ]] || SRCDIR=$TMP/x/usr/lib/firmware/nvidia/$VER
[[ -d $SRCDIR ]] || { echo "error: no lib/firmware/nvidia/$VER in $(basename "$deb")" >&2; exit 1; }

DEST=$OUTDIR/nvidia/$VER
rm -rf "$DEST"; mkdir -p "$DEST"
if [[ $KEEP_ALL == 1 ]]; then
    cp -a "$SRCDIR/." "$DEST/"
else
    [[ -f $SRCDIR/gsp_ga10x.bin ]] || { echo "error: gsp_ga10x.bin not in $(basename "$deb")" >&2; exit 1; }
    cp "$SRCDIR/gsp_ga10x.bin" "$DEST/"
fi
echo "$(basename "$deb") ${src%% *}/$deb" > "$DEST/.source"
echo "== done: $DEST"
for f in "$DEST"/*; do
    echo "   $(basename "$f")  $(du -h "$f" | cut -f1)  sha256 $(sha256sum "$f" | cut -c1-16)"
done

# ── libcuda.so.1, per architecture ───────────────────────────────────────────
# libnvidia-compute-<branch> ships libcuda.so.<ver> with the libcuda.so.1
# symlink; only the file is kept, under the name the runtime looks up.
for arch in "${ARCHES[@]}"; do
    PKG=libnvidia-compute-$BRANCH
    case "$arch" in amd64) gnu=x86_64 ;; arm64) gnu=aarch64 ;; esac
    echo "== $PKG $VER ($arch) from Ubuntu $SERIES"
    src=$(find_deb "$PKG" "$arch" "$VER") || { echo "error: $PKG $VER not found for $arch in $SERIES" >&2; exit 1; }
    deb=${src#* }
    fetch_deb "$src" "$TMP/lib-$arch.deb"
    rm -rf "$TMP/l-$arch"; dpkg-deb -x "$TMP/lib-$arch.deb" "$TMP/l-$arch"
    lib=$(find "$TMP/l-$arch" -type f -name "libcuda.so.$VER" | head -n1)
    [[ -n $lib ]] || { echo "error: libcuda.so.$VER not in $(basename "$deb")" >&2; exit 1; }
    DEST=$LIBDIR/$gnu
    rm -rf "$DEST"; mkdir -p "$DEST"
    cp "$lib" "$DEST/libcuda.so.1"
    echo "$(basename "$deb") ${src%% *}/$deb" > "$DEST/.source"
    echo "== done: $DEST"
    echo "   libcuda.so.1  $(du -h "$DEST/libcuda.so.1" | cut -f1)  sha256 $(sha256sum "$DEST/libcuda.so.1" | cut -c1-16)"
done
