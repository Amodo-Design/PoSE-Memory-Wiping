# PoSE-Memory-Wiping

GPU-accelerated **Proof of Secure Erasure** for persistent storage: fill every
sector of an NVMe drive with PoSE-DB graph labels generated on one or more NVIDIA
GPUs, fast enough that the drive, not the labeler, is the bottleneck in sight.

This repository is the code behind Amodo Design's write-up [Improving Disk Wiping
Speed for Memory Wipes](https://amododesign.com/notes/2026-09-14-disk-wiping-speed/), a follow-up to
[our first memory-wiping post](https://amododesign.com/notes/2026-07-01-memory-wiping/).
It contains the graph-labeling library, the CUDA labeler, the multi-GPU disk-wipe
benchmark and the planning calculator used to produce the numbers in that post,
together with the daemons and verifier that run the same proof across a whole
server tray: its RAM, disks and GPU memory, and those of its DPU and BMC.

> **This software destroys data.** `tools/disk-wipe-bench` overwrites every
> block device it is pointed at, and the daemons overwrite every disk, RAM
> carve-out and GPU the verifier's plan names. Read the tool's README and
> `NOTICE` before running any of it.

## What is in here

The code falls into five groups: the labeling library everything shares, the
daemons and verifier that prove the erasure of a whole tray, the scripts that
package a daemon as a bootable image, the standalone disk-wiping tools, and the
build support.

A tray run has three roles. The **verifier** runs on the operator's machine. It
connects to the tray host's **source-daemon**, which generates every label, on
its GPUs when it has them and on a CPU thread pool otherwise. The source-daemon
stores the labels for its own RAM, disks and GPU memory, and streams the rest to
a **sink-daemon** on each downstream device (the DPU, the BMC), which only
stores them and answers challenges. All three links speak one protocol, defined
in `pose-sink/include/sink_proto.h` and explained in
[`docs/sink-source-architecture.md`](docs/sink-source-architecture.md).

### Labeling library

The C11 library that computes labels, on a CPU or a GPU, with byte-identical
results. [`docs/labeling-explained.md`](docs/labeling-explained.md) explains
what it computes and where each part lives;
[`docs/common-library.md`](docs/common-library.md) documents the API.

| Path | What it is |
|---|---|
| `pose-common/src/`, `include/` | The CPU labeler (`graph.c`) and the node hash (`hash.c`): keyed BLAKE3 by default, with SHA-256 and an AES PRF as CPU alternatives. |
| `pose-common/src/cuda/` | The CUDA labeler, built with `TARGET_GPU=ON`. Keyed BLAKE3 only. |
| `pose-common/test/` | Unit tests run by `ctest`, including the GPU-against-CPU parity test (skipped where there is no GPU). |
| `pose-common/tools/pose_label.c` | `pose-label`: prints the expected label for any block of a session. For checking a daemon by hand. |
| `pose-common/bench/bench_region.c` | CPU labeling throughput benchmark. |
| `pose-common/vendor/blake3/` | The BLAKE3 team's C implementation, version 1.8.5 (see `NOTICE`). |

### Tray daemons and verifier

| Path | What it is |
|---|---|
| `pose-sink/` | Static library shared by both daemons: the wire protocol codec, the TCP transport, discovery of this machine's RAM carve-outs and disks, label storage and challenge answers, and the self-wipe that ends a daemon's life. |
| `sink-daemon/` | Stores labels it did not compute. It contains no graph or hash code, so it stays small enough to read. Builds for aarch64 (the BlueField-3 DPU), x86_64, and armv7 (an ASPEED BMC, fully static). |
| `source-daemon/` | The tray host's daemon: a sink for its own RAM, disks and GPU memory, and the label source for the downstream sinks it is configured with (`-s NODE=IPV4:PORT`). |
| `pose-init/` | A minimal static PID 1 for a bare initramfs. It mounts the pseudo filesystems, loads kernel modules, configures the network from the kernel command line (`pose.addr=`, `pose.mods=`, and so on) and then `exec`s the daemon, which keeps that plumbing out of the daemon itself. |
| `pose-verifier/` | The verifier, in Rust. `inventory` shows what a tray can label, `plan` writes the default plan as TOML, `run` labels, challenges under the round-trip bound and sends Finish, and `serve` does the same from a web page (`web/`). It links `pose-common` to recompute the challenged labels, and its build generates the protocol constants from `sink_proto.h` so the two sides cannot drift. |

### Boot images: `uki/`

A daemon that is to label a machine's RAM cannot run under that machine's
normal OS, so it is booted as a Unified Kernel Image: one signed EFI binary
holding a kernel, a command line and an initramfs that contains nothing but
`pose-init`, the daemon, and the few kernel modules it needs.

| Path | What it is |
|---|---|
| `uki/build-kernel.sh` | Builds an Ubuntu kernel from `vendor/kernel-src/<kver>/` in the container, using Ubuntu's own config for the flavour with the fragments below merged on top. The result keeps the stock `uname -r` and vermagic. Wrapped by `make kernel-aarch64` and `make kernel-x86_64`. |
| `uki/kconfig-fragments/` | The config changes, applied in name order on every build. They open `/dev/mem` to the daemon and strip what a wipe image never uses: filesystems, virtualisation, wireless, tracing, and unneeded drivers. |
| `uki/build-ofed-modules.sh` | Builds the NVIDIA OFED (DOCA) `mlx5` modules against that kernel, for the BlueField-3 DPU's embedded switch, and signs them with the kernel's module key. |
| `uki/build-nvidia-modules.sh` | Builds the NVIDIA compute modules (`nvidia`, `nvidia-uvm`) against that kernel, for the x86 host's GPUs, and signs them the same way. |
| `uki/build-*-container.sh` | The in-container halves of the three scripts above. Not run directly. |
| `uki/build-initramfs.sh` | Packs the initramfs: `pose-init` as `/init`, the daemon as `/pose-daemon`, the modules, any firmware, and a dynamically linked daemon's loader and libraries. Reproducible, and checks that everything the daemon needs resolves inside the image. |
| `uki/build-uki.sh` | Wraps kernel, initramfs and command line into a UKI with `ukify`, then signs it for Secure Boot and verifies the signature. |
| `uki/build-uki-dpu.sh`, `uki/build-uki-x86-host.sh` | The two complete recipes: module list and load order, network configuration and RAM carve-out for the BlueField-3 DPU and for the x86 GPU host. Each takes the daemon to boot as its argument. The addresses, MACs, PCI IDs and versions in them describe Amodo's test tray and must be edited for any other machine. |
| `uki/generate-keys.sh` | Generates the Secure Boot signing pair into `keys/` (git-ignored). The certificate has to be enrolled in the target's UEFI `db`. |

### Standalone disk-wiping tools

| Path | What it is |
|---|---|
| `tools/disk-wipe-bench/` | The disk wipe measured in the write-up. Streams GPU-generated labels through pinned host memory to raw block devices with `O_DIRECT` sequential writes, schedules K GPUs per disk across any number of disks, sample-verifies the result against a CPU recompute, and reports MiB/s. Includes `run-matrix.sh` for unattended configuration sweeps. |
| `tools/disk-calc/` | Stdlib-only Python planning aid: given one GPU's measured label rate, estimate wipe time for X TB across Y GPUs and find the best GPUs-per-disk split. |

### Build support

| Path | What it is |
|---|---|
| `Makefile` | One target per component and architecture; `make help` lists them. Everything except the verifier builds inside the container. |
| `docker/Dockerfile` | The build image (`make docker`): Ubuntu 24.04 with native and cross toolchains for x86_64, aarch64 and armv7, and the CUDA toolkit, so every target builds from either an amd64 or an arm64 machine. |
| `cmake/` | A toolchain file per target architecture, plus `pose-runtime.cmake` and `gather-runtime.sh`, which collect the dynamic loader and libraries a daemon needs so they can be packed into an initramfs. |
| `vendor/` | Scripts that fetch third-party inputs for boot images: the Ubuntu kernel source, the systemd EFI stub, the NVIDIA GSP firmware and `libcuda`, and the source of the NVIDIA and OFED kernel modules. The kernel builds also land here. |

### Documentation

| Path | What it is |
|---|---|
| `docs/labeling-explained.md` | Plain-language explanation of what a label is, how the graph is built, and how it maps onto the target's address space. |
| `docs/common-library.md` | API and design notes for `pose-common/`. |
| `docs/sink-source-architecture.md` | The verifier, source-daemon and sink-daemon roles, a run step by step, and the wire protocol. |
| `docs/unified-kernel-images.md` | What is in a boot image, building and deploying one, and adapting the recipes to another tray. |

## How it works, briefly

PoSE-DB (Bursuc, Gil-Pons, Mauw, Trujillo-Rasua, 2024,
[arXiv:2401.06626](https://arxiv.org/abs/2401.06626)) turns erasure into a
proof. The prover fills the target with the labels of a depth-robust DAG keyed
by a verifier-supplied seed. **Writing the labels is the erasure**: every prior
byte is overwritten. The verifier then challenges random positions under a
round-trip time bound; a prover that discarded the labels cannot recompute one
in time because each label sits at the end of a long sequential hash chain.

For disks, the labels are computed on GPUs and written sequentially. Each label
depends only on the session seed and its absolute position, so a disk filled by
four GPUs over disjoint sector ranges is byte-for-byte the disk one GPU would
have produced. Parallelism changes the speed, never the bytes.

## Quick start

Everything except the verifier builds in a container, so the build machine
needs only Docker (or Podman) and `make`:

```bash
make docker              # build the pose-build image, once
make pose-common-test    # labeling library and its tests
make help                # every other target
```

Without the container, the CPU library and its tests build on any machine with
a C11 compiler and CMake:

```bash
cmake -S pose-common -B pose-common/build
cmake --build pose-common/build
ctest --test-dir pose-common/build --output-on-failure
```

### Wiping disks with GPUs

The disk-wipe benchmark needs Linux and an NVIDIA GPU. `make disk-wipe-bench`
builds it for x86_64 in the container; on a GPU host with the CUDA toolkit
installed, the same build is:

```bash
cmake -S tools/disk-wipe-bench -B tools/disk-wipe-bench/build \
      -DCMAKE_BUILD_TYPE=Release -DPOSE_CUDA_ARCH=90 \
      -DPOSE_GPU_LABEL_STRATEGY=warp
cmake --build tools/disk-wipe-bench/build -j
```

Then, on the GPU host:

```bash
# Validate the pipeline without writing anything:
sudo tools/disk-wipe-bench/build/disk-wipe-bench --disk /dev/nvmeXn1 --chunk-blocks 131072 --dry-run

# Real wipe of one disk with one GPU, then sample-verify 16 super-chunks:
sudo tools/disk-wipe-bench/build/disk-wipe-bench --disk /dev/nvmeXn1 --chunk-blocks 131072 --confirm --verify 16
```

`POSE_CUDA_ARCH` is 90 for Hopper (H100/H200) and 100 for Blackwell; the
`Makefile` takes it as `CUDA_ARCH`.

See [`tools/disk-wipe-bench/README.md`](tools/disk-wipe-bench/README.md) for
the safety guards, multi-GPU scheduling flags, verification, and what the
numbers do and do not mean.

Then project to fleet scale:

```bash
python3 tools/disk-calc/disk_calc.py --gpu-mibs 620 --gpus 4 --disk-tb 30.72 --disks 8 --disk-mibs 5000
```

### Proving a whole tray

Build the daemons for the machines they run on, and the verifier natively
(it needs a Rust toolchain, CMake and a C compiler):

```bash
make source-daemon-x86_64    # tray host    -> source-daemon/build-x86_64/source-daemon
make sink-daemon-aarch64     # DPU          -> sink-daemon/build-aarch64/sink-daemon
make sink-daemon-armv7       # BMC          -> sink-daemon/build-armv7/sink-daemon
make verifier                # operator     -> pose-verifier/target/release/pose-verifier
```

Start a `sink-daemon` on each downstream device and the `source-daemon` on the
tray host, naming the sinks. Both listen on TCP port 7776 and need root to map
RAM and open disks:

```bash
sink-daemon                                   # on the DPU
source-daemon -s dpu0=192.168.100.2:7776      # on the tray host
```

Then, from the operator's machine:

```bash
pose-verifier inventory HOST    # what the tray can label
pose-verifier plan HOST -o plan.toml
pose-verifier run HOST --plan plan.toml
pose-verifier serve HOST        # the same from a web page on port 8080
```

A run overwrites every target in its plan. The links are plain TCP with no
authentication, and the web page has none either, so keep all of it on a
trusted network.

### Booting a daemon as a signed image

To label RAM, a daemon has to be the only thing running on the machine. The
scripts in `uki/` build a signed image that boots straight into it, from a
kernel, a minimal initramfs and a fixed command line. The two recipes that ship,
`uki/build-uki-x86-host.sh` and `uki/build-uki-dpu.sh`, describe Amodo's test
tray and have to be edited for any other machine.

[`docs/unified-kernel-images.md`](docs/unified-kernel-images.md) is the guide:
the tools the build machine needs, the build step by step, how to deploy an
image, and how to adapt the recipes.

## Security notes

- The GPU labeler implements keyed BLAKE3 only. It is the only hash with
  byte-identical CPU/GPU parity, and the tests and `--verify` mode depend on it.
- `chunk_blocks` sets the graph depth, and with it the adversary's cost to fake
  a label inside the RTT window. Larger is more secure and slower per persisted
  byte. The benchmarks in the write-up use `2^17`.
- The `warp` strategy needs a larger transient scratch pool in GPU memory (about
  25 GiB at `2^17`). For disk wiping that is irrelevant; for wiping the GPU's own
  HBM it is unattested space and needs a separate pass.
- LBA-addressed overwrite cannot reach SSD over-provisioned or remapped blocks,
  nor the drive controller's DRAM. Those need hardware secure erase or firmware
  support and are outside this tool's proof.

## License

MIT. Copyright (c) 2026 Amodo Design Ltd. See [`LICENSE`](LICENSE) and
[`NOTICE`](NOTICE). The vendored BLAKE3 implementation under
`pose-common/vendor/blake3/` is by the BLAKE3 team under its own permissive licenses
(see `pose-common/vendor/blake3/LICENSE`).
