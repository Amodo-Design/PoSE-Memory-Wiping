#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# fetch-ubuntu-kernel-src.sh — fetch the exact Ubuntu kernel source for one
# kernel version into vendor/kernel-src/<kver>/, ready for the docker/Dockerfile
# container (fakeroot debian/rules ... or make bindeb-pkg).  Run it once per
# target kernel:
#
#   vendor/fetch-ubuntu-kernel-src.sh 6.8.0-1030-bluefield   # BlueField-3 DPU (aarch64),
#                                                           the kernel the DOCA 3.5.0
#                                                           ubuntu24.04/arm64-dpu repo ships
#   vendor/fetch-ubuntu-kernel-src.sh 6.8.0-147-generic      # x86_64 host
#
# The version (`uname -r`) is split into base 6.8.0, ABI 147 and flavour
# generic.  The flavour picks the Launchpad git tree: *bluefield* ->
# linux-bluefield, maintained by the ~canonical-kernel-bluefield team
# (https://launchpad.net/~canonical-kernel-bluefield), anything else -> linux
# from the Ubuntu kernel team (~ubuntu-kernel).
#
# That tree's branch for the Ubuntu series (-s, default noble) is probed for
# the upload tag of that flavour and ABI: Ubuntu-<base>-<abi>.* for generic
# (Ubuntu-6.8.0-147.147), otherwise Ubuntu-<flavour>-<base>-<abi>.*
# (Ubuntu-bluefield-6.8.0-1030.35, Ubuntu-lowlatency-6.8.0-147.147.1).  The
# tree is shallow-cloned at that tag, with the full debian/ packaging.
# HWE backport kernels (Ubuntu-hwe-x.y-... tags) are deliberately not matched.
#
# Usage: vendor/fetch-ubuntu-kernel-src.sh [options] KVER
#   -o DIR      output directory        (default vendor/kernel-src)
#   -s SERIES   Ubuntu series            (default noble, i.e. 24.04)
#   -l          list: resolve only, fetch nothing
#   -f          re-fetch even if <outdir>/<kver>/Makefile exists
#   -h          help
#
# Needs: git and network access to git.launchpad.net.  Nothing is run on the targets.

set -euo pipefail
export GIT_TERMINAL_PROMPT=0

OUTDIR=vendor/kernel-src
SERIES=noble
LIST=0
FORCE=0
LP_GIT=https://git.launchpad.net

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "o:s:lfh" opt; do
    case "$opt" in
        o) OUTDIR=$OPTARG ;;
        s) SERIES=$OPTARG ;;
        l) LIST=1 ;;
        f) FORCE=1 ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one kernel version is required, e.g. 6.8.0-147-generic (see -h)" >&2; exit 1; }
KVER=$1
BASE=$(cut -d- -f1 <<<"$KVER")
ABI=$(cut -d- -f2 <<<"$KVER")
FLAVOUR=$(cut -d- -f3- <<<"$KVER")

# Tags are Ubuntu-<version> for generic and Ubuntu-<flavour>-<version> otherwise.
if [[ $FLAVOUR == generic ]]; then
    TAGPAT="Ubuntu-$BASE-$ABI.*"
else
    TAGPAT="Ubuntu-$FLAVOUR-$BASE-$ABI.*"
fi
case "$FLAVOUR" in
    *bluefield*) TREE="~canonical-kernel-bluefield/ubuntu/+source/linux-bluefield" ;;
    *)           TREE="~ubuntu-kernel/ubuntu/+source/linux" ;;
esac

cd "$(dirname "$(readlink -f "$0")")/.."

# Halt if previoudly cloned
echo "== $KVER"
DEST="$OUTDIR/$KVER"
if [[ $FORCE -eq 0 && -f $DEST/Makefile ]]; then
    echo "  already present at $DEST ($(cat "$DEST/.kernel-commit" 2>/dev/null || echo untracked)); use -f to re-fetch"
    exit 0
fi

# Resolve the upload tag (newest match if the ABI was uploaded more than once).
URL="$LP_GIT/$TREE/+git/$SERIES"
echo "  probing ${TREE##*/}/$SERIES ..."
# a missing tree is an error from ls-remote, a missing tag is just no output
TAG=$(git ls-remote --tags --refs "$URL" "refs/tags/$TAGPAT" 2>/dev/null \
      | sed 's#.*refs/tags/##' | sort -V | tail -n 1) || true
if [[ -z $TAG ]]; then
    echo "error: no tag $TAGPAT for $KVER under $TREE in series $SERIES" >&2
    echo "       check the ABI with 'apt-cache policy linux-image-$KVER' on the target, or the series with -s" >&2
    exit 1
fi
echo "  found $TAG in $URL"
[[ $LIST -eq 1 ]] && exit 0

# Remove directory and clone fresh
[[ -d $DEST ]] && rm -rf "$DEST"
mkdir -p "$OUTDIR"
git clone --depth 1 --branch "$TAG" "$URL" "$DEST"

# Record tag + commit for reproducibility (same convention as scripts/fetch-kernel.sh).
echo "$TAG $(git -C "$DEST" rev-parse HEAD) $URL" > "$DEST/.kernel-commit"
echo "  pinned: $(cat "$DEST/.kernel-commit")"
echo "  done: $DEST ($(du -sh "$DEST" | cut -f1))"
