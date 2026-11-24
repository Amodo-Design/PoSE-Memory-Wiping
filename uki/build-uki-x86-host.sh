#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-uki-x86-host.sh — build a signed UKI for the x86_64 host that boots
# straight into one daemon (sink or source): the generic kernel built by
# uki/build-kernel.sh, the modules the host needs (network), the static
# x86_64 PID 1 shim, the daemon given on the command line, and the Secure
# Boot signing pair.
#
# Inputs
#   vendor/kernel-build/<kver>/bzImage                vendor/fetch-ubuntu-kernel-src.sh + uki/build-kernel.sh
#   vendor/kernel-build/<kver>/modules/<kver>/kernel  same build, plain .ko
#   vendor/kernel-build/<kver>/modules/<kver>/updates/dkms  nvidia.ko + nvidia-uvm.ko,
#                                                     uki/build-nvidia-modules.sh
#   vendor/nvidia-firmware/nvidia/<ver>/gsp_ga10x.bin GSP firmware for the H200s,
#   vendor/nvidia-libcuda/x86_64/libcuda.so.1         and the CUDA driver library the
#                                                     source-daemon dlopen()s for its GPU
#                                                     sessions: vendor/fetch-nvidia.sh <ver>
#   pose-init/build-x86_64/pose-init                  static x86_64 build of pose-init/
#   DAEMON (argument)                                 any x86_64 daemon, e.g.
#                                                     sink-daemon/build-x86_64/sink-daemon
#
# Outputs (uki/build/, gitignored)
#   uki/build/initrd-x86-<daemon>.img
#   uki/build/pose-x86-<daemon>.efi
#
# DEBUG=1 builds variants whose pose.onfail parks on the console instead of
# rebooting.  NET picks the network interfaces (see below).  CARVEOUT is the
# kernel parameter that hides RAM for a sink to label (see below); CARVEOUT= (set
# but empty) builds without one, so a sink offers only disks.
#
# Usage: uki/build-uki-x86-host.sh DAEMON

set -euo pipefail
[[ $# -eq 1 ]] || { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
DAEMON=$(readlink -f "$1")
NAME=$(basename "$DAEMON")
cd "$(dirname "$(readlink -f "$0")")/.."

KVER=6.8.0-147-generic
KBUILD=vendor/kernel-build/$KVER
KERNEL=$KBUILD/bzImage
MODDIR=$KBUILD/modules/$KVER
FWDIR=vendor/nvidia-firmware
GSPFW=$FWDIR/nvidia/595.99.02/gsp_ga10x.bin
LIBCUDA=vendor/nvidia-libcuda/x86_64/libcuda.so.1
INIT=pose-init/build-x86_64/pose-init
OUT=uki/build

# Modules, in load order.
#   bnxt_en        the Broadcom NIC the host boots from (no module dependencies).
#   mlxfw, mlx5_core
#                  the BlueField-3 host PF, AKA the host end of the DPU's eswitch.
#   nvidia, nvidia-uvm
#                  the H200 compute driver (NVIDIA open kernel module, rebuilt and
#                  signed against this kernel).  nvidia.ko request_firmware()s the
#                  GSP blob from /lib/firmware/nvidia/<ver>/ when it initialises a
#                  GPU, so that tree is packed into the initramfs too (-F).
MODULES=(
    kernel/drivers/net/ethernet/broadcom/bnxt/bnxt_en.ko
    kernel/drivers/net/ethernet/mellanox/mlxfw/mlxfw.ko
    kernel/drivers/net/ethernet/mellanox/mlx5/core/mlx5_core.ko
    updates/dkms/nvidia.ko
    updates/dkms/nvidia-uvm.ko
)
POSE_MODS=bnxt_en,mlxfw,mlx5_core,nvidia,nvidia_uvm

# Command line.  ttyS0 (COM1) is the only console: the BMC carries it as
# Serial-over-LAN (docs/unified-kernel-images.md).
#
# NET picks the host's interfaces, one pose.addr each, all selected by MAC:
#   LAN      the Broadcom port cabled to the LAN, at the address the host's OS
#            gets from DHCP; it carries the default route, so a verifier on
#            another subnet can reach the daemon.
#   ESWITCH  the BlueField-3 host PF (ens9f0np0 on stock Ubuntu), the host end of
#            the DPU's eswitch; the DPU's UKI sink answers at 10.99.0.2 on it.
# NET=eswitch (default) configures both, and points a source daemon downstream at
# the DPU's UKI sink as node "dpu0": the source reads POSE_SINK_<node>=IPV4:PORT
# from its environment (a sink daemon ignores it).  NET=lan configures only the
# LAN and no downstream: use it when the DPU is not running its eswitch UKI,
# because pose-init fails the boot if an interface it was given never appears.
LAN="pose.addr=mac:30:56:0f:9c:50:d9,192.168.103.12/24,192.168.103.1"
ESWITCH="pose.addr=mac:8c:91:3a:d4:34:40,10.99.0.1/30"
DPU_SINK="POSE_SINK_dpu0=10.99.0.2:7776"
ENV="POSE_FINAL_ACTION=reboot"
# Only the source-daemon labels GPUs: it gets libcuda.so.1 (88 MB) and
# POSE_GPUS=1, which makes a GPU-less start (no driver, no firmware, no
# libcuda) a failed boot rather than a tray with its HBM quietly missing.
GPU=0; [[ $NAME == source-daemon ]] && GPU=1
[[ $GPU == 1 ]] && ENV="$ENV,POSE_GPUS=1"
NET=${NET:-eswitch}
case "$NET" in
    eswitch) NETCFG="$LAN $ESWITCH"; ENV="$ENV,$DPU_SINK" ;;
    lan)     NETCFG="$LAN" ;;
    *)       echo "error: NET must be eswitch or lan" >&2; exit 1 ;;
esac
# RAM carve-out.  On x86, mem=N drops every RAM range above physical address N
# (it is not an amount).  This host has ~2.4 GB below 4 GiB and one range from
# 4 GiB to ~97 GiB, so mem=6G leaves the kernel ~4.4 GB and hides ~91 GiB,
# which the source finds as SMBIOS-declared DRAM the kernel does not own and
# labels through /dev/mem.  Sized 2026-11-10 from a real run's accounting
# (1100 MiB allocated at startup: kernel, initramfs, nvidia.ko + GSP firmware)
# plus the daemon's measured peak on a GPU-generated session at chunk_blocks
# 2^18 (2.6 GiB: the host-side scaffold topology build and the generator's
# 256 MiB pinned slice) and ~2 MiB of page tables per GiB of carve-out
# mapped: ~3.9 GiB needed, ~0.5 GiB spare.  Before the topology build was
# slimmed the peak was 6.4 GiB and even mem=8G would have been too small.
# The kernel docs pair mem= with memmap= so PCI BARs cannot land in the hidden
# RAM.  That is not needed here: BARs go inside the ACPI host bridge windows,
# which on this host are below 4 GiB or above 1 TiB.  memmap= would also mark
# the hole Reserved, and the sink treats every Reserved entry as claimed.
CARVEOUT=${CARVEOUT-mem=6G}
BASE="console=ttyS0,115200n8 ignore_loglevel panic=10"
ONFAIL=reboot; [[ ${DEBUG:-0} == 1 ]] && ONFAIL=park
POSE="pose.mods=$POSE_MODS $NETCFG pose.wait=20 pose.onfail=$ONFAIL pose.env=$ENV"

need() { [[ -f $1 ]] || { echo "error: missing $1" >&2; echo "       $2" >&2; exit 1; }; }
need "$KERNEL" "vendor/fetch-ubuntu-kernel-src.sh $KVER && uki/build-kernel.sh $KVER"
for m in "${MODULES[@]}"; do
    case "$m" in
        updates/dkms/*) need "$MODDIR/$m" "uki/build-nvidia-modules.sh $KVER" ;;
        *)              need "$MODDIR/$m" "uki/build-kernel.sh $KVER (is the driver still =m in the fragments?)" ;;
    esac
done
need "$GSPFW" "vendor/fetch-nvidia.sh 595.99.02"
largs=()
if [[ $GPU == 1 ]]; then
    need "$LIBCUDA" "vendor/fetch-nvidia.sh 595.99.02"
    largs=(-l "$LIBCUDA")
fi

echo "== initramfs"
margs=(); for m in "${MODULES[@]}"; do margs+=(-m "$MODDIR/$m"); done
uki/build-initramfs.sh -i "$INIT" -d "$DAEMON" "${margs[@]}" -F "$FWDIR" "${largs[@]}" -o "$OUT/initrd-x86-$NAME.img" | grep '^=='

echo "== uki"
uki/build-uki.sh -i "$OUT/initrd-x86-$NAME.img" -c "$BASE${CARVEOUT:+ $CARVEOUT} $POSE" \
    -o "$OUT/pose-x86-$NAME.efi" "$KVER" | grep '^=='
