#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-uki.sh — wrap a kernel built by uki/build-kernel.sh, an initramfs built
# by uki/build-initramfs.sh and a command line into one Unified Kernel Image
# (a signed PE binary that UEFI boots directly), for any kernel flavour on
# either architecture.
#
# KVER names the kernel: vendor/kernel-build/<kver>/Image (aarch64) or
# bzImage (x86_64), whichever the build produced, and that choice decides the
# systemd EFI stub: vendor/efi-stubs/linuxaa64.efi.stub or linuxx64.efi.stub,
# The initramfs is checked to carry a PID 1 of the same architecture, because
# a mismatch produces a UKI that dies in firmware with no diagnostics.  The
# result is Secure Boot signed with KEYDIR/mok.key + KEYDIR/mok.crt and
# verified with sbverify, unless -u is given.
#
# Usage: uki/build-uki.sh -i INITRD -c CMDLINE -o OUT [-s KEYDIR | -u] KVER
#   -i INITRD   initramfs image                   (e.g. uki/initrd-dpu.img)
#   -c CMDLINE  kernel command line, one string
#   -o OUT      UKI to write                      (e.g. uki/pose-dpu-daemon-nomem.efi)
#   -s KEYDIR   sign with KEYDIR/mok.key and KEYDIR/mok.crt (PEM) (default: keys)
#   -u          leave the UKI unsigned
#   -h          help
#
# Paths are relative to the repo root.  Needs: ukify, file, objdump; sbsign
# and sbverify unless -u.

set -euo pipefail

INITRD=""
CMDLINE=""
OUT=""
KEYDIR=keys
KBUILD=vendor/kernel-build

usage() { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while getopts "i:c:o:s:uh" opt; do
    case "$opt" in
        i) INITRD=$OPTARG ;;
        c) CMDLINE=$OPTARG ;;
        o) OUT=$OPTARG ;;
        s) KEYDIR=$OPTARG ;;
        u) KEYDIR="" ;;
        h) usage 0 ;;
        *) usage 1 ;;
    esac
done
shift $((OPTIND - 1))
[[ $# -eq 1 ]] || { echo "error: exactly one kernel version is required, e.g. 6.8.0-1030-bluefield (see -h)" >&2; exit 1; }
KVER=$1
[[ -n $INITRD && -n $CMDLINE && -n $OUT ]] || { echo "error: -i, -c and -o are all required (see -h)" >&2; exit 1; }
for t in ukify file objdump; do
    command -v "$t" >/dev/null || { echo "error: $t is not installed" >&2; exit 1; }
done

cd "$(dirname "$(readlink -f "$0")")/.."

# Signing is the default, so check for its tools and keys before building
if [[ -n $KEYDIR ]]; then
    for t in sbsign sbverify; do
        command -v "$t" >/dev/null || { echo "error: $t is not installed (needed to sign; -u builds unsigned)" >&2; exit 1; }
    done
    [[ -f $KEYDIR/mok.key && -f $KEYDIR/mok.crt ]] || { echo "error: missing $KEYDIR/mok.key or $KEYDIR/mok.crt; run uki/generate-keys.sh, or place the Secure Boot signing pair in $KEYDIR/ (-u builds unsigned)" >&2; exit 1; }
fi

# The kernel image fixes the architecture and therefore the stub
if   [[ -f $KBUILD/$KVER/Image ]];   then KERNEL=$KBUILD/$KVER/Image;   ARCH=aarch64; EFIARCH=aa64
elif [[ -f $KBUILD/$KVER/bzImage ]]; then KERNEL=$KBUILD/$KVER/bzImage; ARCH=x86_64;  EFIARCH=x64
else echo "error: no Image or bzImage under $KBUILD/$KVER; build it first: uki/build-kernel.sh $KVER" >&2; exit 1
fi
STUB=vendor/efi-stubs/linux$EFIARCH.efi.stub
[[ -f $STUB ]] || { echo "error: missing $STUB; run vendor/fetch-efi-stub.sh $([[ $ARCH == aarch64 ]] && echo arm64 || echo amd64)" >&2; exit 1; }
[[ -f $INITRD ]] || { echo "error: initramfs not found: $INITRD" >&2; exit 1; }

# The initramfs must carry a PID 1 built for the same architecture
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
(cd "$TMP" && zcat "$OLDPWD/$INITRD" 2>/dev/null | cpio -i --quiet init 2>/dev/null) || true
[[ -f $TMP/init ]] || { echo "error: $INITRD has no /init (not a gzip cpio built by uki/build-initramfs.sh?)" >&2; exit 1; }
case "$(file -L "$TMP/init")" in
    *aarch64*|*"ARM aarch64"*) IARCH=aarch64 ;;
    *x86-64*)                  IARCH=x86_64 ;;
    *)                         IARCH=unknown ;;
esac
[[ $IARCH == "$ARCH" ]] || { echo "error: kernel $KVER is $ARCH but the initramfs /init is $IARCH" >&2; exit 1; }

# Build; ukify is chatty on stderr even on success, so show its output only if it fails
mkdir -p "$(dirname "$OUT")"
out=$(ukify build --linux="$KERNEL" --initrd="$INITRD" --cmdline="$CMDLINE" --stub="$STUB" \
      --uname="$KVER" --output="$OUT" 2>&1) || { echo "$out" >&2; echo "error: ukify failed" >&2; exit 1; }

# Sign and verify
if [[ -n $KEYDIR ]]; then
    sbsign --key "$KEYDIR/mok.key" --cert "$KEYDIR/mok.crt" --output "$OUT.signed" "$OUT" > /dev/null 2>&1
    mv "$OUT.signed" "$OUT"
    sbverify --cert "$KEYDIR/mok.crt" "$OUT" > /dev/null || { echo "error: signature verification failed on $OUT" >&2; exit 1; }
    SIGNED="signed with $KEYDIR/mok.crt"
else
    SIGNED="unsigned"
fi

echo "== built: $OUT ($(du -h "$OUT" | cut -f1), $ARCH $KVER, stub $(basename "$STUB"), $SIGNED)"
objdump -h "$OUT" | awk '$2 ~ /^\./ {printf "   %-12s %9d bytes\n", $2, strtonum("0x"$3)}'
