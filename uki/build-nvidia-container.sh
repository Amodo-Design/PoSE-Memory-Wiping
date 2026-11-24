#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-nvidia-container.sh — the in-container half of uki/build-nvidia-modules.sh.
# Runs inside the pose-build image with the repo mounted at /work.
# Not meant to be run directly; build-nvidia-modules.sh sets the environment below.
#
#   SRC       kernel source tree   (/work/vendor/kernel-src/<kver>)
#   OUT       output dir           (/work/vendor/kernel-build/<kver>)
#   PRISTINE  nvidia source tree   (/work/vendor/nvidia-src/nvidia-<ver>)
#   KVER      A.B.C-ABI-flavour
#   NVVER     nvidia version string (for logging)
#   DEBARCH   target Debian arch: arm64 | amd64
#
# Builds ONLY nvidia.ko + nvidia-uvm.ko (compute), strips + signs them into
# OUT/modules/<kver>/updates/dkms with the kernel's own module key.

set -euo pipefail
for v in SRC OUT PRISTINE KVER DEBARCH; do
    [[ -n ${!v:-} ]] || { echo "error: $v is not set; run this via uki/build-nvidia-modules.sh" >&2; exit 1; }
done
JOBS=$(nproc)
OBJ=$OUT/obj
BUILD=$OUT/nvidia
DEST=$OUT/modules/$KVER/updates/dkms

case "$DEBARCH" in
    arm64) ARCH=arm64;  TRIPLET=aarch64-linux-gnu ;;
    amd64) ARCH=x86_64; TRIPLET=x86_64-linux-gnu ;;
esac
CROSS=""; [[ $(dpkg --print-architecture) == "$DEBARCH" ]] || CROSS="$TRIPLET-"
GCC=$(sed -n 's/^export gcc?=//p' "$SRC/debian/rules.d/0-common-vars.mk"); GCC=${GCC:-gcc}

echo "== NVIDIA ${NVVER:-?} (nvidia + nvidia-uvm) against $KVER ($DEBARCH, CC=${CROSS}$GCC, -j$JOBS)"
# the nvidia build writes into its tree: build from a copy of the pristine source
rm -rf "$BUILD"; cp -a "$PRISTINE" "$BUILD"
cd "$BUILD"
# nvidia's own Makefile drives kbuild.  NV_KERNEL_MODULES limits it to the compute
# pair; SYSSRC/SYSOUT point at our O= kernel (sources in src, config+symvers in obj).
make -j"$JOBS" modules \
    NV_KERNEL_MODULES="nvidia nvidia-uvm" \
    SYSSRC="$SRC" SYSOUT="$OBJ" KERNEL_UNAME="$KVER" \
    ARCH="$ARCH" CROSS_COMPILE="$CROSS" CC="${CROSS}$GCC" LD="${CROSS}ld.bfd" \
    IGNORE_XEN_PRESENCE=1 IGNORE_CC_MISMATCH=1 CONFIG_X86_KERNEL_IBT= \
    > make.log 2>&1 || { grep -nE 'error:|Error [0-9]|cc1:|fatal' make.log | head -30; echo "make failed (see $BUILD/make.log)"; exit 1; }

mkdir -p "$DEST"
SIGHASH=$(sed -n 's/^CONFIG_MODULE_SIG_HASH="\(.*\)"/\1/p' "$OBJ/.config"); SIGHASH=${SIGHASH:-sha512}
SIGKEY=$OBJ/certs/signing_key.pem; SIGX509=$OBJ/certs/signing_key.x509
for mod in nvidia nvidia-uvm; do
    ko=$(find . -name "$mod.ko" | head -1)
    [[ -n $ko && -f $ko ]] || { echo "error: $mod.ko was not built"; exit 1; }
    d=$DEST/$mod.ko
    "${CROSS}strip" --strip-debug -o "$d" "$ko"
    if [[ -f $SIGKEY && -f $SIGX509 ]]; then
        "$OBJ/scripts/sign-file" "$SIGHASH" "$SIGKEY" "$SIGX509" "$d"
    else
        echo "WARNING: $SIGKEY missing; $mod.ko left unsigned (Secure Boot will reject it)"
    fi
done
echo "== done: $DEST"
for mod in nvidia nvidia-uvm; do
    f="$DEST/$mod.ko"
    printf '   %-12s %6s  vermagic=%-50s signer=%s\n' "$mod.ko" "$(du -h "$f" | cut -f1)" \
        "$(modinfo -F vermagic "$f")" "$(modinfo -F signer "$f" 2>/dev/null || echo UNSIGNED)"
done
echo "   deps: nvidia-uvm -> $(modinfo -F depends "$DEST/nvidia-uvm.ko")"
