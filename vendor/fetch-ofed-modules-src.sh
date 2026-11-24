#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# fetch-ofed-modules-src.sh — fetch the NVIDIA OFED kernel module source for one DOCA
# release, which uki/build-ofed-modules.sh builds the mlx5 modules from.
#
# It is the mlnx-ofed-kernel-dkms package from NVIDIA's public DOCA repository
# for that release, unpacked.  The package is architecture-independent source
# (_all.deb), so the copy in the x86_64 repository serves an aarch64 DPU too;
# the DPU's own repository (arm64-dpu) ships prebuilt modules and no source.
#
# DOCA is the release the DPU itself runs, e.g. 3.5.0: the OFED modules must
# match it.  The OFED version (2607.0.100-1 for DOCA 3.5.0) is read from the
# repository's package index, and the download is checked against the SHA256
# listed there.
#
# Output:
#   vendor/ofed-src/usr/src/mlnx-ofed-kernel-<ver>/           the source tree
#   vendor/ofed-src/usr/src/mlnx-ofed-kernel-<ver>/.source    the package it came from
#   vendor/ofed-src/usr/src/ofa_kernel-<ver>/                 as shipped beside it
#
# Usage: vendor/fetch-ofed-modules-src.sh [-d DISTRO] [-r REPO] [-l] DOCA
#   DOCA        DOCA release, e.g. 3.5.0
#   -d DISTRO   distribution directory in the DOCA repository  (default ubuntu24.04)
#   -r REPO     repository under it to take the package from   (default x86_64)
#   -l          list: resolve only, fetch nothing
#   -h          help
#
# Needs: curl, dpkg-deb, sha256sum, network access to linux.mellanox.com.

set -euo pipefail

DISTRO=ubuntu24.04
REPO=x86_64
LIST_ONLY=0
OUTDIR=vendor/ofed-src
BASE=https://linux.mellanox.com/public/repo/doca
PKG=mlnx-ofed-kernel-dkms

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "d:r:lh" opt; do
    case "$opt" in
        d) DISTRO=$OPTARG ;;
        r) REPO=$OPTARG ;;
        l) LIST_ONLY=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one DOCA release is required, e.g. 3.5.0 (see -h)" >&2; exit 1; }
DOCA=$1
[[ $DOCA =~ ^[0-9A-Za-z][0-9A-Za-z._-]*$ ]] || { echo "error: '$DOCA' is not a DOCA release (want e.g. 3.5.0)" >&2; exit 1; }
for t in curl dpkg-deb sha256sum; do
    command -v "$t" >/dev/null || { echo "error: $t is not installed" >&2; exit 1; }
done

cd "$(dirname "$(readlink -f "$0")")/.."
URL=$BASE/$DOCA/$DISTRO/$REPO
echo "== $PKG from DOCA $DOCA ($DISTRO/$REPO)"

# The repository is flat: its package index sits next to pool/
echo "   probing $URL/Packages ..."
index=$(curl -fsSL "$URL/Packages") || { echo "error: no package index at $URL/Packages (check the DOCA release, -d and -r)" >&2; exit 1; }
read -r ver deb sha < <(awk -v p="$PKG" \
    '$0=="Package: "p{f=1} f&&/^Version:/{v=$2} f&&/^Filename:/{n=$2} f&&/^SHA256:/{s=$2} f&&/^$/{exit} END{print v, n, s}' <<<"$index") || true
[[ -n ${ver:-} && -n ${deb:-} && -n ${sha:-} ]] || { echo "error: $PKG is not in $URL/Packages (the arm64-dpu repository has no source; use -r x86_64)" >&2; exit 1; }
echo "   $PKG $ver: $deb"
[[ $LIST_ONLY == 1 ]] && exit 0

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "   downloading $(basename "$deb")"
curl -fsSL -o "$TMP/ofed.deb" "$URL/$deb"
echo "$sha  $TMP/ofed.deb" | sha256sum -c --quiet - || { echo "error: $(basename "$deb") does not match the SHA256 in the package index" >&2; exit 1; }
dpkg-deb -x "$TMP/ofed.deb" "$TMP/x"
SRCDIR=$TMP/x/usr/src/mlnx-ofed-kernel-$ver
[[ -f $SRCDIR/configure ]] || { echo "error: no usr/src/mlnx-ofed-kernel-$ver in $(basename "$deb")" >&2; exit 1; }

# Everything the package puts in usr/src, replacing a previous fetch of the
# same version and leaving other versions alone.
mkdir -p "$OUTDIR/usr/src"
for d in "$TMP"/x/usr/src/*; do
    rm -rf "$OUTDIR/usr/src/$(basename "$d")"
    cp -a "$d" "$OUTDIR/usr/src/"
done
DEST=$OUTDIR/usr/src/mlnx-ofed-kernel-$ver
echo "$(basename "$deb") $URL/$deb" > "$DEST/.source"
echo "== done: $DEST ($(du -sh "$DEST" | cut -f1), OFED $ver for DOCA $DOCA, from $(basename "$deb"))"
