#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-ofed-container.sh — the in-container half of uki/build-ofed-modules.sh.
# Runs inside the pose-build image with the repo mounted at /work.
# Not meant to be run directly; build-ofed-modules.sh sets the environment below.
#
#   SRC         kernel source tree        (/work/vendor/kernel-src/<kver>)
#   OUT         output dir                (/work/vendor/kernel-build/<kver>)
#   PRISTINE    unpacked OFED source      (/work/vendor/ofed-src/usr/src/mlnx-ofed-kernel-<ver>)
#   KVER        A.B.C-ABI-flavour
#   OFEDVER     OFED version string (for logging)
#   DEBARCH     target Debian arch: arm64 | amd64
#   CONFIG_ONLY 1 = stop after ./configure
#
# Steps: copy the pristine OFED tree -> ./configure against our O= kernel ->
# make -> strip + sign mlx_compat/mlxfw/mlxdevm/mlx5_core into OUT/modules/.../updates/dkms.

set -euo pipefail
for v in SRC OUT PRISTINE KVER DEBARCH CONFIG_ONLY; do
    [[ -n ${!v:-} ]] || { echo "error: $v is not set; run this via uki/build-ofed-modules.sh" >&2; exit 1; }
done
JOBS=$(nproc)
OBJ=$OUT/obj
BUILD=$OUT/ofed
DEST=$OUT/modules/$KVER/updates/dkms

case "$DEBARCH" in
    arm64) ARCH=arm64;  TRIPLET=aarch64-linux-gnu ;;
    amd64) ARCH=x86_64; TRIPLET=x86_64-linux-gnu ;;
esac
CROSS=""; [[ $(dpkg --print-architecture) == "$DEBARCH" ]] || CROSS="$TRIPLET-"
GCC=$(sed -n 's/^export gcc?=//p' "$SRC/debian/rules.d/0-common-vars.mk"); GCC=${GCC:-gcc}
export ARCH CROSS_COMPILE="$CROSS" CC="$CROSS$GCC"

echo "== OFED ${OFEDVER:-?} against $KVER ($DEBARCH, CC=$CC, -j$JOBS)"
# configure modifies the tree in place: build from a copy of the pristine unpack
rm -rf "$BUILD"; cp -a "$PRISTINE" "$BUILD"
cd "$BUILD"
# --with-linux-obj: our kernel is an O= build, so headers+config are in obj, sources in src
./configure --kernel-version="$KVER" --kernel-sources="$SRC" --with-linux-obj="$OBJ" \
    --with-mlx5_core-mod --with-mlxfw-mod --with-mlxdevm-mod --with-njobs="$JOBS" \
    > configure.log 2>&1 || { tail -40 configure.log; echo "configure failed (see $BUILD/configure.log)"; exit 1; }
echo "   configured ($BUILD/configure.log)"
[[ $CONFIG_ONLY == 1 ]] && exit 0

make -j"$JOBS" > make.log 2>&1 || { grep -nE 'error|Error' make.log | head -30; echo "make failed (see $BUILD/make.log)"; exit 1; }

MODS=(compat/mlx_compat.ko
      drivers/net/ethernet/mellanox/mlxfw/mlxfw.ko
      net/mlxdevm/mlxdevm.ko
      drivers/net/ethernet/mellanox/mlx5/core/mlx5_core.ko)
mkdir -p "$DEST"
# The kernel enforces module signatures under Secure Boot and signs its own
# in-tree modules (MODULE_SIG_ALL=y) with certs/signing_key.pem, whose cert sits
# in the kernel's builtin trusted keyring.  Sign the OFED modules with the same
# key and hash so the running kernel trusts them exactly as it does in-tree ones.
SIGHASH=$(sed -n 's/^CONFIG_MODULE_SIG_HASH="\(.*\)"/\1/p' "$OBJ/.config"); SIGHASH=${SIGHASH:-sha512}
SIGKEY=$OBJ/certs/signing_key.pem; SIGX509=$OBJ/certs/signing_key.x509
for m in "${MODS[@]}"; do
    [[ -f $m ]] || { echo "error: $m was not built"; exit 1; }
    d=$DEST/$(basename "$m")
    "${CROSS}strip" --strip-debug -o "$d" "$m"
    if [[ -f $SIGKEY && -f $SIGX509 ]]; then
        "$OBJ/scripts/sign-file" "$SIGHASH" "$SIGKEY" "$SIGX509" "$d"
    else
        echo "WARNING: $SIGKEY missing; $(basename "$m") left unsigned (Secure Boot will reject it)"
    fi
done
echo "== done: $DEST"
for m in "${MODS[@]}"; do
    f="$DEST/$(basename "$m")"
    printf '   %-14s %6s  signer=%-32s depends=%s\n' "$(basename "$f")" "$(du -h "$f" | cut -f1)" \
        "$(modinfo -F signer "$f" 2>/dev/null || echo UNSIGNED)" "$(modinfo -F depends "$f")"
done
echo "   kernel vermagic: $(modinfo -F vermagic $OBJ/net/psample/psample.ko 2>/dev/null || cat $OBJ/include/config/kernel.release)"
