#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# gather-runtime.sh — copy the dynamic loader and every shared library a
# binary needs, found in the compiler's own sysroot, into a root-relative tree
# that uki/build-initramfs.sh packs with -R.  Run as a POST_BUILD step by
# cmake/pose-runtime.cmake, so the runtime always comes from the same glibc the
# binary was just linked against.
#
#   OUT/lib/<triplet>/<lib>     every DT_NEEDED library, recursively, and the loader
#   OUT/<PT_INTERP path>        relative symlink to the loader, if it lives elsewhere
#
# A library the binary dlopen()s at runtime (libcuda.so.1) is not DT_NEEDED and
# comes from the driver instead (vendor/nvidia-libcuda, packed by the UKI
# script), but what *it* needs from glibc must be here too: name those as LIB
# arguments and their closure is gathered as well.
#
# Usage: cmake/gather-runtime.sh BINARY OUTDIR CC [LIB...]
#   CC   the C compiler the binary was linked with: its -dumpmachine names the
#        triplet and its -print-file-name finds each library in its sysroot
#   LIB  further libraries to gather, by soname (e.g. libm.so.6)

set -euo pipefail
[[ $# -ge 3 ]] || { echo "usage: $0 BINARY OUTDIR CC [LIB...]" >&2; exit 1; }
BIN=$1; OUT=$2; CC=$3; shift 3
EXTRA=("$@")

rm -rf "$OUT"
interp=$(readelf -lW "$BIN" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
if [[ -z $interp ]]; then
    echo "gather-runtime: $(basename "$BIN") is statically linked, no runtime needed"
    exit 0
fi
TRIPLET=$("$CC" -dumpmachine)
LIBDIR=$OUT/lib/$TRIPLET
mkdir -p "$LIBDIR"

find_lib() {                       # absolute path of $1 in the compiler's sysroot
    local p; p=$("$CC" -print-file-name="$1")
    [[ $p == /* && -e $p ]] || { echo "gather-runtime: $1 not found by $CC" >&2; exit 1; }
    readlink -f "$p"
}
needed() { readelf -dW "$1" | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p'; }

declare -A seen=()
queue=("$BIN")
# the loader first, by its basename, then everything reachable through DT_NEEDED
# from the binary and from the extra libraries
loader=$(basename "$interp")
cp -L "$(find_lib "$loader")" "$LIBDIR/$loader"; seen[$loader]=1
for lib in "${EXTRA[@]}"; do
    [[ -n ${seen[$lib]:-} ]] && continue
    seen[$lib]=1
    cp -L "$(find_lib "$lib")" "$LIBDIR/$lib"
    queue+=("$LIBDIR/$lib")
done
while ((${#queue[@]})); do
    f=${queue[0]}; queue=("${queue[@]:1}")
    for lib in $(needed "$f"); do
        [[ -n ${seen[$lib]:-} ]] && continue
        seen[$lib]=1
        cp -L "$(find_lib "$lib")" "$LIBDIR/$lib"
        queue+=("$LIBDIR/$lib")
    done
done

# the kernel execs the loader at PT_INTERP: link that path to the copy
if [[ $(dirname "$interp") != "/lib/$TRIPLET" ]]; then
    mkdir -p "$OUT$(dirname "$interp")"
    ln -sfn "$(realpath -m --relative-to="$OUT$(dirname "$interp")" "$LIBDIR/$loader")" "$OUT$interp"
fi
echo "gather-runtime: $(basename "$BIN") -> $OUT: interpreter $interp, $(ls "$LIBDIR" | tr '\n' ' ')"
