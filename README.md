# PoSE-Memory-Wiping

GPU-accelerated **Proof of Secure Erasure** for persistent storage: fill every
sector of an NVMe drive with PoSE-DB graph labels generated on one or more NVIDIA
GPUs, fast enough that the drive, not the labeler, is the bottleneck in sight.

This repository is the code behind Amodo Design's write-up *Improving Disk Wiping
Speed for Memory Wipes*, a follow-up to
[our first memory-wiping post](https://amododesign.com/notes/2026-07-01-memory-wiping/).
It contains the graph-labeling library, the CUDA labeler, the multi-GPU disk-wipe
benchmark, and the planning calculator used to produce the numbers in that post.

> **This software destroys data.** `tools/disk-wipe-bench` overwrites every
> block device it is pointed at. Read its README and `NOTICE` before running it.

## What is in here

| Path | What it is |
|---|---|
| `common/` | The PoSE-DB Protocol 3 labeler (C11). `graph.c` builds the depth-robust DAG and labels it in place; `hash.c` is the keyed BLAKE3 node hash. Unit tests under `test/`. |
| `common/src/cuda/` | The CUDA labeler. The host computes the seed-independent graph topology once with `graph.c`; the kernel runs keyed BLAKE3 per node. Labels are **byte-identical** to the CPU path, so a verifier can challenge GPU-written storage with the CPU code. Four scheduling strategies are selectable at build time: `level` (parity reference), `block`, `thread`, and `warp` (fastest measured). |
| `tools/disk-wipe-bench/` | The wipe itself. Streams GPU-generated labels through pinned host memory to raw block devices with `O_DIRECT` sequential writes, schedules K GPUs per disk across any number of disks, sample-verifies the result against a CPU recompute, and reports MiB/s. Includes `run-matrix.sh` for unattended configuration sweeps. |
| `tools/disk-calc/` | Stdlib-only Python planning aid: given one GPU's measured label rate, estimate wipe time for X TB across Y GPUs and find the best GPUs-per-disk split. |
| `docs/labeling-explained.md` | Plain-language explanation of what a label is, how the graph is built, and how it maps onto the target's address space. |
| `docs/common-library.md` | API and design notes for `common/`. |

Not included: the device-side erasure daemon, the verifier, the RAM and GPU-HBM
session code, and the boot images from the wider project. This repository is
the disk-wiping path only.

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

CPU library and tests, on any machine with a C11 compiler and CMake:

```bash
make common-test
```

The disk-wipe benchmark needs Linux, a CUDA toolkit, and an NVIDIA GPU. On the
GPU host:

```bash
cmake -S tools/disk-wipe-bench -B tools/disk-wipe-bench/build \
      -DCMAKE_BUILD_TYPE=Release -DPOSE_CUDA_ARCH=90 \
      -DPOSE_GPU_LABEL_STRATEGY=warp
cmake --build tools/disk-wipe-bench/build -j

# Validate the pipeline without writing anything:
sudo tools/disk-wipe-bench/build/disk-wipe-bench --disk /dev/nvmeXn1 --chunk-blocks 131072 --dry-run

# Real wipe of one disk with one GPU, then sample-verify 16 super-chunks:
sudo tools/disk-wipe-bench/build/disk-wipe-bench --disk /dev/nvmeXn1 --chunk-blocks 131072 --confirm --verify 16
```

`POSE_CUDA_ARCH` is 90 for Hopper (H100/H200) and 100 for Blackwell. If nvcc is
not on the build machine, `make docker && make disk-wipe-bench` builds the same
binary inside the CUDA devel container from `Dockerfile`.

See [`tools/disk-wipe-bench/README.md`](tools/disk-wipe-bench/README.md) for
the safety guards, multi-GPU scheduling flags, verification, and what the
numbers do and do not mean.

Then project to fleet scale:

```bash
python3 tools/disk-calc/disk_calc.py --gpu-mibs 620 --gpus 4 --disk-tb 30.72 --disks 8 --disk-mibs 5000
```

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
`common/vendor/blake3/` is by the BLAKE3 team under its own permissive licenses
(see `common/vendor/blake3/LICENSE`).
