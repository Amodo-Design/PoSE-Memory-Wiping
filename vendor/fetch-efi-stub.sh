#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# fetch-efi-stub.sh — fetch the systemd EFI stub that uki/build-uki.sh wraps a
# kernel with, for a given architecture.
#
# Usage: vendor/fetch-efi-stub.sh [-s SERIES] amd64|arm64
#   -s SERIES   Ubuntu series to take the package from  (default noble, i.e. 24.04)
#   -h          help
#
# Needs: curl, dpkg-deb, network access to archive.ubuntu.com / ports.ubuntu.com.

set -euo pipefail

SERIES=noble
OUTDIR=vendor/efi-stubs

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "s:h" opt; do
    case "$opt" in
        s) SERIES=$OPTARG ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one architecture is required: amd64 or arm64 (see -h)" >&2; exit 1; }
case "$1" in
    amd64) DEBARCH=amd64; MIRROR=http://archive.ubuntu.com/ubuntu; STUB=linuxx64.efi.stub ;;
    arm64) DEBARCH=arm64; MIRROR=http://ports.ubuntu.com/ubuntu-ports; STUB=linuxaa64.efi.stub ;;
    *) echo "error: architecture must be amd64 or arm64" >&2; exit 1 ;;
esac
for t in curl dpkg-deb; do
    command -v "$t" >/dev/null || { echo "error: $t is not installed" >&2; exit 1; }
done

cd "$(dirname "$(readlink -f "$0")")/.."
echo "== $STUB from Ubuntu $SERIES $DEBARCH"

# Newest systemd-boot-efi in the updates pocket, falling back to the release pocket
deb=""
for pocket in "$SERIES-updates" "$SERIES"; do
    echo "   probing $pocket/universe ..."
    deb=$(curl -fsSL "$MIRROR/dists/$pocket/universe/binary-$DEBARCH/Packages.gz" | zcat \
          | awk '/^Package: systemd-boot-efi$/{p=1} p&&/^Filename:/{print $2; exit} p&&/^$/{p=0}') || true
    [[ -n $deb ]] && break
done
[[ -n $deb ]] || { echo "error: systemd-boot-efi not found for $DEBARCH in $SERIES" >&2; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
echo "   downloading $(basename "$deb")"
curl -fsSL -o "$TMP/stub.deb" "$MIRROR/$deb"
dpkg-deb -x "$TMP/stub.deb" "$TMP/x"
mkdir -p "$OUTDIR"
cp "$TMP/x/usr/lib/systemd/boot/efi/$STUB" "$OUTDIR/$STUB"
echo "$(basename "$deb") $MIRROR/$deb" > "$OUTDIR/$STUB.source"
echo "== done: $OUTDIR/$STUB ($(du -h "$OUTDIR/$STUB" | cut -f1), from $(basename "$deb"))"
