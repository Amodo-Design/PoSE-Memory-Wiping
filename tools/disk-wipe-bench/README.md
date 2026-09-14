# disk-wipe-bench — GPU-driven PoSE-DB disk wipe + throughput benchmark

Performs the **real disk wipe** and measures how fast it runs. It fills every
addressable sector of the chosen block device(s) with PoSE-DB graph labels
generated on the GPU — the same labels the CPU path
(`pose_graph_label_region_pooled_inplace` in `common/src/graph.c`) and the
verifier produce — and reports achieved MiB/s per GPU and in aggregate.

The labeling *is* the erasure: writing the graph's output-set labels overwrites
all prior contents and is exactly what a later verifier would challenge. This
tool stops before the challenge/proof phase; it exists to benchmark the wipe
itself so the parallel-wipe design can be sized on measured numbers rather than
the `tools/disk-calc` model.

It does **not** reimplement the algorithm. Labels come from the validated
`pose_graph_label_hbm` (`common/src/cuda`), byte-identical to the CPU in-place
path. The only new code here is host-side plumbing: GPU→host copy, O_DIRECT
sequential writes, multi-GPU scheduling, and safety guards.

## ⚠ This destroys data

Every real run overwrites the target device(s) from sector 0. It is a wipe.
Guards that are always on:

- **Only** devices named explicitly with `--disk` are touched — no scanning, no
  globbing, no auto-selection.
- Any device that is mounted, carries a mounted partition, or backs `/` is
  **refused** (checked against `/proc/mounts`, matched at whole-disk granularity
  so `/dev/nvme0n1` is refused when `/dev/nvme0n1p2` is mounted). Targets are
  `realpath()`'d first, so a `/dev/disk/by-id/...` symlink is checked as the
  device it points to.
- Real writes open the device **`O_EXCL`**, the same guard `wipefs`/`mkfs` use:
  the kernel refuses the open (`EBUSY`) if the disk or any of its partitions is
  held by a mounted filesystem, device-mapper (LVM / LUKS), md RAID, or swap —
  the cases a `/proc/mounts` scan cannot see (e.g. an Ubuntu root on LVM).
- The exact device list and byte ranges are printed before any write.
- Real writes require `--confirm`. Without it the tool refuses, unless
  `--dry-run` is given.
- `--dry-run` does everything **except** the disk write (label + device→host
  copy only) — safe on any CUDA box and the way to validate the pipeline first.

Real writes to a raw block device need **root** (O_DIRECT). Run under `sudo`.

**The default seed is public.** Without `--seed-hex`, labels are derived from a
fixed seed compiled into the binary (see `g_seed` in `disk_wipe_bench.cu`; it is
not a secret). That is fine for measuring throughput and for `--verify`, and the
overwrite is still a full overwrite, but it is **not a challengeable erasure**: a
verifier only gains assurance from labels keyed by a seed it chose and kept
private until the session started. For a real PoSE session, generate a fresh
random 32-byte seed and pass it with `--seed-hex`.

## Build

Needs a CUDA toolkit (nvcc) and CMake ≥ 3.18. **Use the `warp` strategy**; it is
the measured fastest (620 MiB/s per H200 vs 185 MiB/s for `block`, see below):

```bash
rm -rf tools/disk-wipe-bench/build          # CMake caches the strategy; clear it when switching
cmake -S tools/disk-wipe-bench -B tools/disk-wipe-bench/build \
      -DCMAKE_BUILD_TYPE=Release -DPOSE_CUDA_ARCH=90 \
      -DPOSE_GPU_LABEL_STRATEGY=warp -DPOSE_GPU_WARP_MIN_BLOCKS=3 \
      -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc
cmake --build tools/disk-wipe-bench/build -j
```

Output: `tools/disk-wipe-bench/build/disk-wipe-bench`, a dynamically linked
Linux binary — run it directly on the host so devices and `sudo` are
straightforward.

- `POSE_CUDA_ARCH`: `90` = Hopper (H100/H200), `100` = Blackwell (GB200). Default 90.
- `POSE_GPU_LABEL_STRATEGY`: `level` | `block` | `thread` | `warp`. Default `level`
  (the byte-parity reference), so check that the configure step prints
  `labeling strategy warp`.
- `POSE_GPU_WARP_MIN_BLOCKS`: `3` | `4` (warp only). To try the higher-occupancy
  variant, rebuild with `4` and add `-DCMAKE_CUDA_FLAGS="-Xptxas -v"`; the
  `k_label_chunk_warp` line in the build output must report 0 bytes spill
  stores/loads, otherwise stay on 3.
- `-DCMAKE_CUDA_COMPILER` is only needed if nvcc is not on `PATH`.

The same build is wrapped by the top-level `Makefile` (`make disk-wipe-bench
GPU_STRATEGY=warp`), which runs it inside the CUDA devel container from the
repo `Dockerfile` when you do not have nvcc on the build machine.

After any rebuild that touches the hash code, run a `--verify` wipe against a
loop device before trusting the numbers (see *Verifying the labels*).

## Run

Always validate with `--dry-run` first (no writes):

```bash
# 1. Pipeline check — labels + D2H, writes nothing. Safe anywhere with a GPU.
sudo ./build/disk-wipe-bench --disk /dev/nvme1n1 --chunk-blocks 131072 --dry-run

# 2. Real wipe of one disk with one GPU:
sudo ./build/disk-wipe-bench --disk /dev/nvme1n1 --chunk-blocks 131072 --confirm

# 3. Real wipe of 4 disks, one GPU each, in parallel:
sudo ./build/disk-wipe-bench \
     --disk /dev/nvme1n1 --disk /dev/nvme2n1 --disk /dev/nvme3n1 --disk /dev/nvme4n1 \
     --mode disk-per-gpu --gpus 0,1,2,3 --chunk-blocks 131072 --confirm

# 4. Real wipe of one disk with all 4 GPUs cooperating (disjoint sector ranges):
sudo ./build/disk-wipe-bench --disk /dev/nvme1n1 \
     --mode gpus-per-disk --gpus 0,1,2,3 --chunk-blocks 131072 --confirm

# 5. 4 GPUs (of 8), 2 disks, 2 GPUs each, both disks in parallel:
sudo ./build/disk-wipe-bench --disk /dev/nvme1n1 --disk /dev/nvme2n1 \
     --gpus 0,1,2,3 --gpus-per-disk 2 --chunk-blocks 131072 --confirm

# Quick timed sample without wiping the whole drive (still real writes to the cap):
sudo ./build/disk-wipe-bench --disk /dev/nvme1n1 --per-disk-mib 16384 --confirm
```

## Verifying the labels

You don't need a separate verifier project to confirm the wipe wrote correct
labels. There are two distinct notions of "verify":

- **Correctness** — *did the GPU write the right PoSE-DB labels?* Read labels back
  from the disk and recompute the expected label on the CPU (the byte-identical
  `graph.c` path), then compare. This is what a verifier's challenge checks, minus
  the network and RTT timing. Built into this tool via `--verify`.
- **Distance-bounding proof** — the verifier's challenge protocol. It proves a
  *live* device couldn't have relayed/recomputed within the round-trip bound, and it
  **labels-and-proves in one operation** (labeling *is* the wipe). It does not
  verify a pre-labeled disk — pointing it at your drive would re-wipe it — so it's
  not the tool for checking a finished benchmark wipe.

For a finished wipe, use the correctness check:

```bash
# after a wipe, add --verify N to sample-check N super-chunks:
sudo ./build/disk-wipe-bench --disk "$DISK" --chunk-blocks 131072 --confirm --verify 256

# or check an already-wiped disk later (read-only, no re-wipe) — must pass the
# SAME --chunk-blocks and --seed-hex the wipe used:
sudo ./build/disk-wipe-bench --disk "$DISK" --chunk-blocks 131072 --verify-only --verify 512
```

It reads `N` super-chunks spread across the wiped range (always the first and
last), recomputes each on the CPU for the same seed and chunk index, and reports
`X/Y sampled super-chunks match`. Any mismatch prints the offending super-chunk
and block and exits non-zero. A full match across a good spread is strong evidence
the whole labeled region is correct (each super-chunk is an independent graph, so
a systematic labeler bug shows up in the sample). `--verify-only` defaults to 128
samples if `--verify N` isn't given. This checks the labels are the correct PoSE
output set; it is a **sampled** proof, not a bit-for-bit audit of every
super-chunk — raise `N` for more coverage (cost is one CPU graph recompute per
sample).

## Scheduling

Pick the GPU pool with `--gpus`, then choose how many GPUs team up per disk with
`--gpus-per-disk K`. That gives `T = floor(pool / K)` disks wiped **in parallel**,
each by a team of `K` GPUs (within a team the disk's sector range is split into K
disjoint contiguous sub-ranges). It's the same K axis the `disk-calc` tool sweeps.

The two `--mode` values are just aliases for the extremes of that axis:

| Setting | K | Disks in parallel (pool of N) | When |
|---|---|---|---|
| `--mode disk-per-gpu` (default) | 1 | N | many disks, one GPU each |
| `--gpus-per-disk K` | K | N / K | the middle ground, e.g. 4 GPUs → 2 disks × 2 GPUs |
| `--mode gpus-per-disk` | N | 1 (sequential) | few disks, done fastest one at a time |

`--gpus-per-disk` overrides `--mode`. If the pool isn't divisible by K the
remainder GPUs sit idle (reported in the banner); disks are assigned to teams
largest-first for balance. Each team GPU runs one long-lived worker that carries
its sub-range on every disk the team owns, so the scaffold topology and HBM
window are built once per GPU for the whole run, not once per disk.

All settings write byte-identical labels regardless of which GPU produces a given
super-chunk: the label depends only on `(seed, absolute chunk index)`, and each
worker is handed the correct absolute offset. A cooperative multi-GPU wipe and a
single-GPU wipe of the same disk produce the same bytes.

## Options

| Flag | Default | Meaning |
|---|---|---|
| `--disk PATH` | — | target block device; repeatable; **required** |
| `--gpus LIST` | all visible | CUDA ordinals to use, e.g. `0,1,2,3` |
| `--gpus-per-disk K` | from `--mode` | K GPUs per disk; `floor(pool/K)` disks in parallel. Overrides `--mode` |
| `--mode MODE` | `disk-per-gpu` | alias: `disk-per-gpu` (K=1) or `gpus-per-disk` (K=pool) |
| `--chunk-blocks N` | 4096 | graph size, power of two in `[1, 2^20]`; use the session's value (must make the super-chunk `N×32` a multiple of the disk sector, e.g. 131072) |
| `--per-disk-mib N` | 0 = whole device | cap bytes written per disk for a quick test |
| `--hbm-window-mib N` | 0 = 16 GiB (capped at 30 % of free VRAM) | HBM label buffer per GPU; must hold at least one super-chunk per resident warp for the `warp` strategy (12.4 GiB at `cb = 131072` on 132 SMs) or the labeler is starved |
| `--stage-mib N` | 1024 | pinned host D2H/write slice size |
| `--dry-run` | off | label + D2H only; no disk writes |
| `--confirm` | off | required for real (destructive) writes |
| `--verify N` | 0 | after the wipe, sample-check N super-chunks (read-back + CPU recompute) |
| `--verify-only` | off | skip the wipe; only verify existing labels (read-only) |
| `--verbose` | off | disable the live dashboard; print raw per-window labeler logs |
| `--seed-hex HEX` | fixed public seed | 64 hex chars = 32-byte session seed; required for a challengeable erasure (see above) |

## Live status

By default, on an interactive terminal the tool shows a live, in-place dashboard
(htop-style) that refreshes ~2×/second — one row per GPU plus an aggregate — while
the shared labeler's own per-window log lines are silenced so nothing interleaves:

```
=== disk-wipe-bench ===
schedule      : 2 GPU(s)/disk, 2 disk(s) in parallel
...
GPU 0  nvme1n1      [########------------]  41.2%   186.0 MiB/s  ETA 12:03  write
GPU 1  nvme1n1      [########------------]  41.0%   185.4 MiB/s  ETA 12:07  write
GPU 2  nvme2n1      [#######-------------]  39.8%   184.1 MiB/s  ETA 12:20  write
GPU 3  nvme2n1      [#######-------------]  39.6%   183.7 MiB/s  ETA 12:22  write
ALL    9.7 / 30.7 GiB [#######-------------]  40.4%   739.2 MiB/s  ETA 06:05  elapsed 04:12
```

The rows are **responsive**: each redraw sizes to the current terminal width, so
on resize nothing wraps or garbles, and as the window narrows fields drop in
priority order (progress bar → ETA → rate → disk name) rather than clipping — down
to `GPU 0  41.2% write` and finally `G0  41%` in a very narrow pane.

When stdout is **not** a TTY (piped, `tee`, `nohup`, a log file), it falls back to
a compact summary line every ~5 s instead of redraw escapes, so logs stay clean.
Pass `--verbose` to disable the dashboard entirely and print the raw per-window
labeler logs (the old behaviour) — useful for debugging. The final `=== results ===`
table (per-GPU MiB/s + label/d2h/write split) prints after the run in all modes.

## Output

Per-GPU `MiB/s` with the time split across label / D2H / write, plus the
aggregate over wall-clock. Feed the per-GPU number to `tools/disk-calc`
(`--gpu-mibs`) to project fleet-scale wipe times.

## What it measures (and doesn't)

- **Representative:** real GPU labeling at the given `chunk_blocks`, real
  device→host copy, real O_DIRECT sequential writes to the actual medium. The
  seed-independent scaffold topology is built and uploaded **once per worker** and
  reused across every HBM window (via `pose_graph_hbm_topo_*`), so a multi-window
  wipe doesn't pay the topology-build cost repeatedly — the aggregate rate tracks
  the per-window label rate.
- **Chunk size and strategy matter more than HBM bandwidth.** Measured with
  `block` at `cb = 131072`: H100 SXM 179 MiB/s, H200 NVL 185 MiB/s per window
  (same binary), despite the H200's 43 % higher HBM bandwidth. Nsight Compute
  showed HBM at ~5 % of peak, ALUs at ~19 %, and ~56 % of stall cycles at the
  per-level block barrier; an SM clock sweep split the time into ~2/3 SM-cycle
  bound (hash + barrier) and ~1/3 fixed exposed memory latency. The labeler is
  barrier/latency bound, not bandwidth bound — do not extrapolate across GPUs by
  bandwidth ratio. `warp` (`-DPOSE_GPU_LABEL_STRATEGY=warp`) removes the block
  barrier, and vectorising the label loads/stores (two `uint4` per label instead
  of 32 byte stores, each of which was its own L2 write transaction) removed the
  L2 pressure: **H200 NVL 620 MiB/s per window with `warp` + vector I/O**, parity
  verified 256/256, versus 185 MiB/s for the original `block`. `level` is the
  byte-parity reference and starves on the deep graph spine. Always benchmark at
  the `cb` and on the GPU you'll deploy, and re-profile before extrapolating.
- **Label buffer** is capped at the smaller of ~30 % of free VRAM (or
  `--hbm-window-mib`) and the largest range the worker will process, so a small
  `--per-disk-mib` test doesn't needlessly reserve VRAM on a shared machine.
- **Not overlapped (v1):** within one GPU worker, an HBM window is fully labeled,
  then copied and written, then the next window starts — label and write are not
  pipelined. At the original ~200 MiB/s/GPU label rate this cost ~1 %; at the
  `warp` rate (~620 MiB/s/GPU) the write phase is ~7 % of worker time against a
  fast target and would be more against a slower NVMe, so a pipelined writer is
  now worth revisiting. It still surfaces a disk write ceiling in
  `gpus-per-disk` (K concurrent writers queue on a saturated drive, inflating the
  reported `write` time). A pipelined writer thread is a later addition if the
  write fraction ever grows large.
- A trailing region smaller than one super-chunk (`chunk_blocks × 32` bytes) at
  the very end of a device is left untouched (labels are only ever written in
  whole super-chunks).
  Full over-provisioned/remapped-sector coverage still needs a hardware secure
  erase (a documented limitation of LBA-addressed overwrite).
- No challenge/proof phase, no verifier, no RAM/HBM sessions — disk only.
