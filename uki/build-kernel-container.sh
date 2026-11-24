#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-kernel-container.sh — the in-container half of uki/build-kernel.sh.
# Runs inside the pose-build image with the repo mounted at /work.
# Not meant to be run directly; build-kernel.sh sets the environment below.
#
#   SRC         kernel source tree           (/work/vendor/kernel-src/<kver>)
#   OUT         output dir                   (/work/vendor/kernel-build/<kver>)
#   KVER        A.B.C-ABI-flavour, also used as KERNELRELEASE (uname -r)
#   DEBARCH     target Debian arch: arm64 | amd64, or empty to pick from the flavour
#   FRAGS       kconfig fragments to merge, space-separated /work/... paths, in order
#   CONFIG_ONLY 1 = stop after writing obj/.config
#
# Steps: read the tree's packaging metadata -> pick arch/toolchain -> export
# Ubuntu's flavour config and merge fragments -> olddefconfig -> build image +
# modules -> modules_install (stripped, plain .ko) -> copy artefacts to OUT.

set -euo pipefail
for v in SRC OUT KVER CONFIG_ONLY; do
    [[ -n ${!v:-} ]] || { echo "error: $v is not set; run this via uki/build-kernel.sh" >&2; exit 1; }
done
read -r -a FRAGS <<<"${FRAGS:-}"
cd "$SRC"
JOBS=$(nproc)
O=$OUT/obj
STAGE=$OUT/.stage
REL=$KVER
ABI=$(cut -d- -f2 <<<"$KVER")        # 1030 -> PKG_ABI
FLAVOUR=$(cut -d- -f3- <<<"$KVER")   # e.g. generic, bluefield, or bluefield-64k -> Ubuntu config

# Bluefield is always aarch64, but default to x86 otherwise
if [[ -z ${DEBARCH:-} ]]; then
    case "$FLAVOUR" in *bluefield*) DEBARCH=arm64 ;; *) DEBARCH=amd64 ;; esac
fi
case "$DEBARCH" in
    arm64) ARCH=arm64;  TRIPLET=aarch64-linux-gnu; IMG=Image ;;
    amd64) ARCH=x86_64; TRIPLET=x86_64-linux-gnu;  IMG=bzImage ;;
    *) echo "error: -a must be arm64 or amd64 (got '$DEBARCH')" >&2; exit 1 ;;
esac

# Use cross compilation toolchain if we're on a foreign architecture
CROSS=""
[[ $(dpkg --print-architecture) == "$DEBARCH" ]] || CROSS="$TRIPLET-"
CC="ccache ${CROSS}gcc"

# Mirrors kmake in debian/rules.d/0-common-vars.mk, but let kbuild generate
# its own build number for KBUILD_BUILD_VERSION:
KMAKE=(make -C "$SRC" O="$O" ARCH="$ARCH" CROSS_COMPILE="$CROSS" CC="$CC" HOSTCC=gcc
       KERNELRELEASE="$REL" CONFIG_DEBUG_SECTION_MISMATCH=y
       CFLAGS_MODULE="-DPKG_ABI=$ABI" PYTHON=python3)

# Find the flavour-specific configuration directory to base our build on
DEBIAN=$(awk -F= '$1=="DEBIAN"{print $2}' debian/debian.env)

echo "== $KVER: $DEBIAN, flavour $FLAVOUR, $DEBARCH, CC=$CC, release $REL, -j$JOBS"

# Ubuntu's flavour config, exactly as debian/rules exports it, then the
# fragments on top
mkdir -p "$O"
python3 debian/scripts/misc/annotations --file "$DEBIAN/config/annotations" \
    --arch "$DEBARCH" --flavour "$FLAVOUR" --export > "$O/.config"
sed -i "s/.*CONFIG_VERSION_SIGNATURE.*/CONFIG_VERSION_SIGNATURE=\"Ubuntu $KVER $(make -s kernelversion)\"/" "$O/.config"
echo "   exported $(grep -c '^CONFIG_' "$O/.config") options from $DEBIAN/config/annotations"
if [[ ${#FRAGS[@]} -gt 0 ]]; then
    printf '   fragment: %s\n' "${FRAGS[@]#/work/}"
    ARCH="$ARCH" scripts/kconfig/merge_config.sh -m -O "$O" "$O/.config" "${FRAGS[@]}" > /dev/null
fi

# Resolve dependencies of the enabled configuration options
"${KMAKE[@]}" olddefconfig > /dev/null

# Report every fragment option whose final value differs: olddefconfig can force
# an option back if a dependency is unmet or another option selects it.
bad=0
for f in "${FRAGS[@]}"; do
    while IFS= read -r line; do
        case "$line" in
            CONFIG_*=n)                opt=${line%%=*}; want="unset" ;;   # "=n" and absence are the same thing
            CONFIG_*=*)                opt=${line%%=*}; want=$line ;;
            "# CONFIG_"*" is not set") opt=${line#\# }; opt=${opt%% *}; want="unset" ;;
            *) continue ;;
        esac
        got=$(grep -E "^$opt=" "$O/.config" || echo unset)
        [[ $got == "$want" ]] || { echo "   WARNING: ${f#/work/} wants '$want' but .config has '$got'"; bad=1; }
    done < "$f"
done
[[ $bad == 1 ]] && echo "   (a fragment option was overridden by olddefconfig; check its dependencies in Kconfig)"
echo "   configured: $O/.config"
[[ $CONFIG_ONLY == 1 ]] && exit 0

# Perform the build
"${KMAKE[@]}" -j"$JOBS" "$IMG" modules

# Install built kernel and modules into the outpyt directory
rm -rf "$STAGE"
"${KMAKE[@]}" INSTALL_MOD_PATH="$STAGE" INSTALL_MOD_STRIP=1 modules_install > /dev/null
rm -f "$STAGE/lib/modules/$REL/build" "$STAGE/lib/modules/$REL/source"
rm -rf "$OUT/modules"; mkdir -p "$OUT/modules"
mv "$STAGE/lib/modules/$REL" "$OUT/modules/$REL"
rm -rf "$STAGE"
cp "$O/arch/$ARCH/boot/$IMG" "$OUT/$IMG"
cp "$O/System.map" "$OUT/System.map"
cp "$O/.config" "$OUT/config"

echo "== done: $OUT"
echo "   $IMG: $(du -h "$OUT/$IMG" | cut -f1)   modules/$REL: $(find "$OUT/modules/$REL" -name '*.ko' | wc -l) modules, $(du -sh "$OUT/modules/$REL" | cut -f1)"
echo "   release: $(cat "$O/include/config/kernel.release")"
