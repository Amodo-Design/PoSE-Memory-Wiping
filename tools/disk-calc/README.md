# disk-calc — GPU-driven disk wipe time calculator

Planning aid for the parallel disk wipe: given the **persisted label output rate of one
GPU**, how long does it take to PoSE-DB-label **X TB of disk** with **Y of that GPU**,
and which parallelism shape is fastest?

To *measure* that rate on real hardware (and actually perform a GPU-driven wipe), use
`tools/disk-wipe-bench`; feed its reported per-GPU MiB/s back into `--gpu-mibs` here.

Its node-count math mirrors `common/src/graph.c`. Stdlib-only Python, no dependencies.

## Quickstart — no flags to remember

```bash
python3 tools/disk-calc/disk_calc.py --serve          # local web form, opens in your browser
python3 tools/disk-calc/disk_calc.py --interactive    # prompts for each value in the terminal
```

`--serve` starts a stdlib HTTP server on `127.0.0.1:8765` (`--serve 0` picks a free port)
with one labelled input box per parameter and help text under each. Results — strategy
cards, the K sweep table, and the full text report — update as you type, and the page
shows the equivalent command line so a result can be reproduced or pasted into a note.
`--interactive` asks each question with its default in brackets (Enter keeps it, `?`
explains it), prints the report, then lets you change single values and re-run.

The same parameters are available as flags for scripting:

```bash
python3 tools/disk-calc/disk_calc.py --gpu-mibs 244.5 --gpus 4 --disk-tb 61.44 --disks 8 --disk-mibs 600 --chunk-blocks 131072
python3 tools/disk-calc/disk_calc.py --gpu-mibs 244.5 --gpus 4 --disk-sizes-tb 7.68,7.68,15.36
python3 tools/disk-calc/disk_calc.py --gpu-mibs 244.5 --gpus 4 --disk-tb 30 --disks 8 --json
```

## The input rate — persisted labels, not hashes

The faithful in-place labeler persists **only the output set O(G)** (`chunk_blocks`
labels per super-chunk) but has to hash the whole transient scaffold to produce them:
`scratch_node_count(cb) / cb` ≈ 136× at `cb = 4096`, 276× at `2^17`, 384× at `2^20`.
On top of that the real CUDA schedule loses time to level synchronisation and occupancy.
A raw hash rate therefore overstates disk-fill speed by two to three
orders of magnitude.

Feed the tool the **persisted-output rate the labeler itself reports**:

```
  GPU wipe: 8192 MiB in 195.2 s = 42.0 MiB/s      # common/src/cuda/label_gpu_common.cu
```

→ `--gpu-mibs 42`. Measure it at the **same `chunk_blocks`** the disk sessions will use:
the GPU's wave width (how many super-chunk scaffolds fit in the HBM work buffer) depends
on `cb`, so the rate is not transferable across chunk sizes. Also measure with a large
`size_mib` so the fixed topology-upload cost is amortised.

If you only have a raw rate, `--gpu-hash-rate H --chunk-blocks CB` derives an
**optimistic upper bound** `H × 32 B / excess(CB)` and labels the result as such.

## Model

Every strategy is a point on one axis: **K GPUs per disk "team"**, `floor(Y / K)` teams
running concurrently, disks queued across teams (longest-first).

| Name | K | Teams | Meaning |
|---|---|---|---|
| **A. gpus-per-disk** | Y | 1 | all GPUs pool their labels onto one disk at a time; disks sequential |
| **B. disk-per-gpu** | 1 | Y | each GPU owns a disk; up to Y disks in flight |
| sweep | 1…Y | ⌊Y/K⌋ | every intermediate split; the best one is marked |

Per-disk write rate is the minimum of the pipelined stages:

```
GPU labeler (K × gpu_mibs × pool_eff) → D2H copy (K × d2h_mibs) → disk write (disk_mibs)
                                       → host aggregate write (host_mibs, shared by all teams)
```

All caps default to unlimited, so the default output is the pure **label-bound**
estimate. Set `--disk-mibs` (per-disk sustained sequential write), `--d2h-mibs`
(PCIe device→host per GPU) and `--host-mibs` (backplane / aggregate NVMe write) to find
where storage becomes the bottleneck — the report names the binding stage for each K.

Pooling K GPUs onto one disk is modelled as linear (`--pool-eff 1.0`) because
super-chunks are independent graphs: a disk can be split among GPUs at super-chunk
granularity with no cross-GPU dependency. Lower `--pool-eff` to budget for the host-side
merge/write scheduling once that path exists.

`--sector-bytes` reproduces the O_DIRECT rounding of the super-chunk up to a
sector multiple; for `cb ≥ 128` with 4 KiB sectors there is no padding.

Disk sizes are **decimal TB** (1e12 bytes), as vendors quote them; the report also shows
TiB.

## Output

Text report by default: inputs, the excess multiplier and implied node hash rate, the
two bounds (one GPU serial; perfect pooling of all GPUs), strategies A and B, an
optional `--gpus-per-disk K` row, and the full K sweep with per-disk rate, binding
stage, total time and GPU utilisation. `--json` emits the same as machine-readable JSON
(`seconds` + `human` per estimate).

## Flags

| Flag | Default | Meaning |
|---|---|---|
| `--serve [PORT]` | — | local web form (default port 8765) |
| `-i`, `--interactive` | — | terminal prompts |
| `--gpu-mibs` | — | measured persisted-output rate of one GPU (MiB/s) |
| `--gpu-hash-rate` | — | alternative: raw node hashes/s of one GPU → upper bound |
| `--gpus` | 4 | number of GPUs |
| `--chunk-blocks` | 4096 | session `chunk_blocks` (excess multiplier, super-chunk size) |
| `--disk-tb` / `--disks` | — / 1 | total decimal TB split across N equal disks |
| `--disk-sizes-tb` | — | explicit per-disk sizes, comma-separated (overrides the above) |
| `--disk-mibs` | 0 (∞) | per-disk sustained sequential write cap |
| `--d2h-mibs` | 0 (∞) | per-GPU device→host copy cap |
| `--host-mibs` | 0 (∞) | aggregate host write cap shared by all disks |
| `--pool-eff` | 1.0 | efficiency when K > 1 GPUs feed one disk |
| `--sector-bytes` | 4096 | logical sector size for O_DIRECT rounding |
| `--gpus-per-disk` | 0 | also report this specific K |
| `--json` | — | JSON output |

## Limitations

- Steady-state throughput only: no per-disk setup, `fsync`, topology upload, or
  challenge-phase time. Add the verifier's session overhead separately.
- The host-side GPU→disk path (D2H + `pwrite`) is implemented in
  `tools/disk-wipe-bench`; this calculator only models it. Measure with that tool, then
  set the caps here from the observed rates.
- Analysis aid only — it does not touch the labeler, the bench, or the build.
