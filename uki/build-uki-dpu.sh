#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# build-uki-dpu.sh — build a signed UKI for the BlueField-3 DPU that boots
# straight into one daemon (sink or source): the bluefield kernel built by
# uki/build-kernel.sh, the modules the DPU needs (consoles, network, storage),
# the static aarch64 PID 1 shim, the daemon given on the command line,
# and the Secure Boot signing pair.
#
# Inputs
#   vendor/kernel-build/<kver>/Image                  vendor/fetch-ubuntu-kernel-src.sh + uki/build-kernel.sh
#   vendor/kernel-build/<kver>/modules/<kver>/kernel        same build, plain .ko
#   vendor/kernel-build/<kver>/modules/<kver>/updates/dkms  NVIDIA OFED mlx5 closure,
#                                                     uki/build-ofed-modules.sh
#   pose-init/build-aarch64/pose-init                 static aarch64 build of pose-init/
#   DAEMON (argument)                                 any aarch64 daemon, e.g.
#                                                     sink-daemon/build-aarch64/sink-daemon
#
# Outputs (uki/build/, gitignored)
#   uki/build/initrd-dpu-<daemon>.img
#   uki/build/pose-dpu-<daemon>.efi  mem=640M: the kernel keeps 640 MiB and the
#                                    rest of DRAM is the carve-out a sink labels.
#
# DEBUG=1 builds variants whose pose.onfail parks on the console instead of
# rebooting.  NET selects the host link; see the NET block below.  The console
# is the UART, watched over the BMC's Serial-over-LAN.
#
# Usage: uki/build-uki-dpu.sh DAEMON

set -euo pipefail
[[ $# -eq 1 ]] || { sed -n '4,/^$/p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 1; }
DAEMON=$(readlink -f "$1")
NAME=$(basename "$DAEMON")
cd "$(dirname "$(readlink -f "$0")")/.."

KVER=6.8.0-1030-bluefield
KBUILD=vendor/kernel-build/$KVER
KERNEL=$KBUILD/Image
MODDIR=$KBUILD/modules/$KVER
INIT=pose-init/build-aarch64/pose-init
OUT=uki/build

# Modules, in load order.  Order matters and none of it is in modules.dep: these
# are runtime (ACPI resource) dependencies inferred from the live DPU's lsmod.
#   sbsa_gwdt      UEFI arms the SBSA watchdog (~20 s to reset) before handing
#                  off; the kernel only pets it once this driver is loaded, so it
#                  goes first or the DPU resets mid-boot.
#   mlxbf-tmfifo   the rshim FIFO transport: tmfifo_net0 (NET=tmfifo), and hvc0,
#                  the rshim console, which the command line does not use.
#   pinctrl/gpio   mlxbf_gige takes its PHY interrupt from gpio-mlxbf3, which
#                  needs pinctrl-mlxbf3; without them it defers probe forever.
#   vitesse        the OOB PHY.
#   mlxbf_gige     the OOB NIC (ACPI MLNXBF17).
#   sdhci, sdhci-pltfm, cqhci, sdhci-of-dwcmshc
#                  the eMMC controller (ACPI MLNXBF30), dependencies first;
#                  mmc_core and mmc_block are built in.
#   mlx_compat, mlxfw, mlxdevm, mlx5_core
#                  the ConnectX-7 PF driver (NVIDIA OFED build, updates/dkms/)
#                  and its closure, in modules.dep order.  Loaded even though
#                  the UKI does not use the port: in DPU mode the host's PF
#                  only works while the Arm eswitch is up (measured 2026-10-29:
#                  no mlx5 on the Arm = host NIC in "FW pre-initializing" until
#                  the Arm comes back).
MODULES=(
    kernel/drivers/watchdog/sbsa_gwdt.ko
    kernel/drivers/platform/mellanox/mlxbf-tmfifo.ko
    kernel/drivers/pinctrl/pinctrl-mlxbf3.ko
    kernel/drivers/gpio/gpio-mlxbf3.ko
    kernel/drivers/net/phy/vitesse.ko
    kernel/drivers/net/ethernet/mellanox/mlxbf_gige/mlxbf_gige.ko
    updates/dkms/mlx_compat.ko
    updates/dkms/mlxfw.ko
    updates/dkms/mlxdevm.ko
    updates/dkms/mlx5_core.ko
    kernel/drivers/mmc/host/sdhci.ko
    kernel/drivers/mmc/host/sdhci-pltfm.ko
    kernel/drivers/mmc/host/cqhci.ko
    kernel/drivers/mmc/host/sdhci-of-dwcmshc.ko
)
POSE_MODS=sbsa_gwdt,mlxbf_tmfifo,pinctrl_mlxbf3,gpio_mlxbf3,vitesse,mlxbf_gige,mlx_compat,mlxfw,mlxdevm,mlx5_core,sdhci,sdhci_pltfm,cqhci,sdhci_of_dwcmshc

# Command line.  ttyAMA0, the physical UART, is the only console: the BMC
# carries it as Serial-over-LAN (docs/unified-kernel-images.md).
#
# NET picks the sink's interfaces, one pose.addr each:
#   ESWITCH  the ConnectX embedded switch: pose.switchdev flips the eswitch to
#            switchdev mode so the host PF representor appears, and the sink takes
#            10.99.0.2 on it, selected by phys_port_name since the representor's
#            MAC is random and its ethN name is probe-order dependent.
#   OOB      the OOB management NIC, picked by MAC; it carries the default route,
#            so a verifier on another subnet can reach the daemon.
#   TMFIFO   the rshim tmfifo link (slow, but needs no host eswitch setup): a
#            virtio-net device mlxbf_tmfifo creates, picked by MAC.
# NET=eswitch (default) configures the eswitch and the OOB NIC.  NET=oob and
# NET=tmfifo configure only that one interface.
ESWITCH="pose.switchdev=0000:03:00.0 pose.addr=port:c1pf0,10.99.0.2/30"
OOB="pose.addr=mac:8c:91:3a:d4:34:54,192.168.101.38/24,192.168.101.1"
TMFIFO="pose.addr=mac:00:1a:ca:ff:ff:01,192.168.100.2/30"
NET=${NET:-eswitch}
case "$NET" in
    eswitch) NETCFG="$ESWITCH $OOB" ;;
    oob)     NETCFG="$OOB" ;;
    tmfifo)  NETCFG="$TMFIFO" ;;
    *)       echo "error: NET must be eswitch, oob or tmfifo" >&2; exit 1 ;;
esac
BASE="earlycon=pl011,0x13010000 console=ttyAMA0 ignore_loglevel fixrtc biosdevname=0 iommu.passthrough=1 panic=10"
ONFAIL=reboot; [[ ${DEBUG:-0} == 1 ]] && ONFAIL=park
POSE="pose.mods=$POSE_MODS $NETCFG pose.wait=20 pose.onfail=$ONFAIL pose.env=POSE_FINAL_ACTION=reboot"

need() { [[ -f $1 ]] || { echo "error: missing $1" >&2; echo "       $2" >&2; exit 1; }; }
need "$KERNEL" "vendor/fetch-ubuntu-kernel-src.sh $KVER && uki/build-kernel.sh $KVER"
for m in "${MODULES[@]}"; do
    case "$m" in
        updates/dkms/*) need "$MODDIR/$m" "uki/build-ofed-modules.sh $KVER" ;;
        *)              need "$MODDIR/$m" "uki/build-kernel.sh $KVER (is the driver still =m in the fragments?)" ;;
    esac
done

echo "== initramfs"
margs=(); for m in "${MODULES[@]}"; do margs+=(-m "$MODDIR/$m"); done
uki/build-initramfs.sh -i "$INIT" -d "$DAEMON" "${margs[@]}" -o "$OUT/initrd-dpu-$NAME.img" | grep '^=='

echo "== uki"
uki/build-uki.sh -i "$OUT/initrd-dpu-$NAME.img" -c "$BASE mem=640M $POSE" \
    -o "$OUT/pose-dpu-$NAME.efi" "$KVER" | grep '^=='
