#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-initramfs.sh — pack the minimal initramfs that goes into a UKI
# (DPU and x86 host alike).  It holds only the following, nothing else:
#
#   /init                pose-init, the static PID 1 shim
#   /pose-daemon         the daemon (sink or source) that pose-init exec()s
#   /lib/modules/*.ko    the kernel modules given with -m, plain .ko files
#   /lib/firmware/...    the firmware tree given with -F, if any (e.g. the NVIDIA
#                        GSP blob, which nvidia.ko request_firmware()s at load)
#   /lib/<triplet>/...   a dynamic daemon's loader and libraries (runtime/ next to
#                        the binary, see below) and any library given with -l,
#                        e.g. libcuda.so.1, which the daemon dlopen()s
#   /dev/{console,kmsg,null,random,urandom}   device nodes, made under fakeroot
#
# Usage: uki/build-initramfs.sh -i INIT -d DAEMON -o OUT [-m MODULE ...] [-F DIR] [-l LIB ...]
#   -i INIT     pose-init binary                  (e.g. pose-init/build-aarch64/pose-init)
#   -d DAEMON   daemon binary, installed as /pose-daemon (e.g. sink-daemon/build-aarch64/sink-daemon)
#   -o OUT      initramfs image to write          (e.g. uki/build/initrd-dpu.img)
#   -m MODULE   kernel module (.ko) to include   (repeatable, in load order)
#   -F DIR      firmware tree; its contents become /lib/firmware (repeatable)
#   -l LIB      shared library to put beside the daemon's own, for one it
#               dlopen()s (repeatable); everything it needs must resolve in the image
#   -h          help
#
# /init must be statically linked.  /pose-daemon may be dynamic
#
# Paths are relative to the current directory.  Needs: cpio, fakeroot, file, gzip, readelf.

set -euo pipefail

INIT=""
DAEMON=""
OUT=""
MODS=()
FWDIRS=()
LIBS=()

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "i:d:o:m:F:l:h" opt; do
    case "$opt" in
        i) INIT=$OPTARG ;;
        d) DAEMON=$OPTARG ;;
        o) OUT=$OPTARG ;;
        m) MODS+=("$OPTARG") ;;
        F) FWDIRS+=("$OPTARG") ;;
        l) LIBS+=("$OPTARG") ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 0 ]] || { echo "error: unexpected argument '$1' (see -h)" >&2; exit 1; }
[[ -n $INIT && -n $DAEMON && -n $OUT ]] || { echo "error: -i, -d and -o are all required (see -h)" >&2; exit 1; }
for t in cpio fakeroot file gzip readelf; do
    command -v "$t" >/dev/null || { echo "error: $t is not installed" >&2; exit 1; }
done

# Sanity check that init is statically linked and both binaries are built for the
# same architecture (the daemon may be dynamic; its closure is checked below)
arch_of() {
    local info; info=$(file -L "$1")
    [[ ${2:-} != static || $info == *"statically linked"* ]] || { echo "error: $1 is not statically linked: $info" >&2; exit 1; }
    case "$info" in
        *aarch64*|*"ARM aarch64"*) echo aarch64 ;;
        *x86-64*)                  echo x86_64 ;;
        *)                         echo unknown ;;
    esac
}
[[ -f $INIT ]]   || { echo "error: init not found: $INIT" >&2; exit 1; }
[[ -f $DAEMON ]] || { echo "error: daemon not found: $DAEMON" >&2; exit 1; }
ARCH=$(arch_of "$INIT" static)
DARCH=$(arch_of "$DAEMON")
[[ $ARCH == "$DARCH" ]] || { echo "error: init is $ARCH but daemon is $DARCH" >&2; exit 1; }

# Stage the tree
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
ROOT=$STAGE/root
mkdir -p "$ROOT"/{dev,proc,sys,lib/modules}
install -m 0755 "$INIT"   "$ROOT/init"
install -m 0755 "$DAEMON" "$ROOT/pose-daemon"

for m in "${MODS[@]}"; do
    [[ -f $m ]] || { echo "error: module not found: $m" >&2; exit 1; }
    base=$(basename "$m")
    [[ $base == *.ko ]] || { echo "error: $m is not a plain .ko module" >&2; exit 1; }
    name=${base%.ko}                   # mlxbf-tmfifo
    cp "$m" "$ROOT/lib/modules/$name.ko"
    # pose.mods=<name> resolves to /lib/modules/<name>.ko literally, and module
    # names spell '-' and '_' interchangeably, so provide the other spelling too.
    for alt in "${name//-/_}" "${name//_/-}"; do
        [[ $alt == "$name" ]] || ln -sf "$name.ko" "$ROOT/lib/modules/$alt.ko"
    done
done

for d in "${FWDIRS[@]}"; do
    [[ -d $d ]] || { echo "error: firmware dir not found: $d" >&2; exit 1; }
    mkdir -p "$ROOT/lib/firmware"
    (cd "$d" && find . -type f ! -path '*/.*' -print0 | xargs -0 -r cp --parents -t "$ROOT/lib/firmware")
done

# A dynamically linked daemon brings its loader and libraries in runtime/ next to
# the binary; it is packed at / (the closure check below makes sure it resolves)
RUNTIME=$(dirname "$DAEMON")/runtime
if [[ -d $RUNTIME ]]; then
    # Copy files and symlinks
    (cd "$RUNTIME" && find . \( -type f -o -type l \) -print0 | xargs -0 -r cp -a --parents -t "$ROOT")
fi

# A dynamic daemon's interpreter must be in the image (it comes from runtime/);
# otherwise pose-init's exec of it fails on the target with a bare ENOENT.
DINTERP=$(readelf -lW "$DAEMON" | sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
if [[ -n $DINTERP && ! -e $ROOT$DINTERP ]]; then
    echo "error: $DAEMON is dynamically linked but its interpreter $DINTERP is not in the image" >&2
    exit 1
fi

# Libraries the daemon dlopen()s go where its runtime put libc (the loader's
# default search path), and must find everything they need there: a
# dlopen() failure on the target is just "cannot open shared object file".
if [[ ${#LIBS[@]} -gt 0 ]]; then
    [[ -n $DINTERP ]] || { echo "error: -l needs a dynamically linked daemon (with a runtime/)" >&2; exit 1; }
    LIBROOT=$(dirname "$(find "$ROOT/lib" -name libc.so.6 | head -n1)")
    [[ -d $LIBROOT ]] || { echo "error: no libc.so.6 in the daemon's runtime to put -l libraries beside" >&2; exit 1; }
    for l in "${LIBS[@]}"; do
        [[ -f $l ]] || { echo "error: library not found: $l" >&2; exit 1; }
        [[ $(arch_of "$l") == "$ARCH" ]] || { echo "error: $l is not a $ARCH library" >&2; exit 1; }
        install -m 0755 "$l" "$LIBROOT/$(basename "$l")"
        for need in $(readelf -dW "$l" | sed -n 's/.*(NEEDED).*Shared library: \[\(.*\)\]/\1/p'); do
            [[ -e $LIBROOT/$need ]] || {
                echo "error: $(basename "$l") needs $need, which is not in the image (gather it into the daemon's runtime)" >&2
                exit 1
            }
        done
    done
fi

# Create device nodes with root ownership using fakeroot
mkdir -p "$(dirname "$OUT")"
fakeroot -- bash -s "$ROOT" "$STAGE/initrd.cpio" "${SOURCE_DATE_EPOCH:-0}" <<'FAKEROOT_EOF'
set -euo pipefail
root=$1; out=$2; epoch=$3
cd "$root"
mknod -m 0600 dev/console c 5  1
mknod -m 0644 dev/kmsg    c 1 11
mknod -m 0666 dev/null    c 1  3
mknod -m 0666 dev/random  c 1  8
mknod -m 0666 dev/urandom c 1  9
chown -R 0:0 .
find . -exec touch -h -d "@$epoch" {} +
find . -mindepth 1 -printf '%P\0' | LC_ALL=C sort -z |
    cpio --null --quiet --reproducible -o -H newc -R 0:0 > "$out"
FAKEROOT_EOF
gzip -9 -n -c "$STAGE/initrd.cpio" > "$OUT"

NFW=0; [[ -d $ROOT/lib/firmware ]] && NFW=$(find "$ROOT/lib/firmware" -type f | wc -l)
DLINK=static; [[ -n $DINTERP ]] && DLINK="dynamic via $DINTERP"
echo "== built: $OUT ($(du -h "$OUT" | cut -f1), $ARCH, ${#MODS[@]} modules, $NFW firmware files, daemon $DLINK)"
cpio -itv < "$STAGE/initrd.cpio" 2>/dev/null | awk '{print "   " $0}'
