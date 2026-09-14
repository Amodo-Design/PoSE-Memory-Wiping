#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""disk-calc — GPU-driven disk wipe time calculator (planning aid).

Estimates how long PoSE-DB labeling of X TB of disk takes when the labels are
produced by Y GPUs, under the two parallelism shapes being considered:

  * gpus-per-disk  — every GPU pools its label output onto ONE disk at a time,
                     disks are wiped sequentially      (K = gpus, teams = 1)
  * disk-per-gpu   — each GPU owns one disk, up to Y disks in flight at once
                     (K = 1, teams = gpus)

Both are points on one axis — K GPUs per disk "team", floor(Y/K) teams running
concurrently — so the tool sweeps every K in [1, Y] and reports the best.

INPUT RATE: the number that matters is NOT the raw hash rate.  The faithful
labeler persists only the output set O(G) (chunk_blocks labels per super-chunk)
but must hash the whole transient scaffold to get them —
scratch_node_count(cb)/cb ≈ 130–384× more hashes than persisted labels, plus the
level-sync / occupancy loss of the real kernel schedule.  The rate to feed in is
the PERSISTED OUTPUT rate the labeler itself reports:

    GPU wipe: <MiB> in <s> = <X> MiB/s        (common/src/cuda/label_gpu_common.cu)

Pass that X as --gpu-mibs, measured at the SAME chunk_blocks the disk sessions
will use (the wave width, and therefore the rate, depends on cb).  If you only
have a raw hash rate, --gpu-hash-rate + --chunk-blocks derives an
optimistic upper bound via the excess-hash multiplier instead.

Data path modelled per disk team (pipelined, throughput = min of the stages):

    GPU labeler (K × gpu_mibs × eff)  →  D2H copy (K × d2h_mibs)  →  disk write
    (disk_mibs)  →  host aggregate write cap (host_mibs shared by all teams)

Every cap defaults to "unlimited" so the default answer is the pure label-bound
estimate; set the caps to see where the disk or PCIe becomes the bottleneck.

The node-count math mirrors common/src/graph.c.

Usage:
    python3 tools/disk-calc/disk_calc.py --gpu-mibs 42 --gpus 4 --disk-tb 30 --disks 8
    python3 tools/disk-calc/disk_calc.py --gpu-mibs 42 --gpus 4 --disk-sizes-tb 7.68,7.68,15.36
    python3 tools/disk-calc/disk_calc.py --gpu-mibs 42 --gpus 4 --disk-tb 30 --disks 8 \\
        --disk-mibs 3000 --json
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from functools import lru_cache

# ── constants mirrored from common/include (source of truth) ───────────────────
POSE_HASH_BYTES = 32               # hash.h  — label size in bytes
POSE_CHUNK_BLOCKS = 4096           # graph.h — default blocks per chunk
POSE_GRAPH_MAX_M = 1 << 20  # graph.h — max supported chunk_blocks

MIB = 1024.0 * 1024.0
TB = 1e12                          # vendors quote disks in decimal TB


# ── node-count math (mirrors common/src/graph.c exactly) ───────────────────────
def parameter_n(m: int) -> int:
    return 0 if m <= 1 else max(0, (m - 1).bit_length() - 1)


def butterfly_node_count(dim: int) -> int:
    return (dim + 1) << dim


@lru_cache(maxsize=None)
def connected_node_count(level: int) -> int:
    if level == 0:
        return 1
    return 2 * connected_node_count(level - 1) + 2 * butterfly_node_count(level - 1)


@lru_cache(maxsize=None)
def standalone_node_count(level: int) -> int:
    if level == 0:
        return 1
    return (standalone_node_count(level - 1)
            + butterfly_node_count(level - 1)
            + connected_node_count(level - 1))


def scratch_node_count(chunk_blocks: int) -> int:
    """Transient scaffold nodes hashed per persisted super-chunk."""
    return 2 * standalone_node_count(parameter_n(chunk_blocks) + 1)


def excess_multiplier(chunk_blocks: int) -> float:
    """Scaffold hashes per persisted output label = scratch_node_count(cb)/cb."""
    return scratch_node_count(chunk_blocks) / chunk_blocks


def is_valid_chunk_blocks(cb: int) -> bool:
    return 1 <= cb <= POSE_GRAPH_MAX_M and (cb & (cb - 1)) == 0


# ── parameters ─────────────────────────────────────────────────────────────────
@dataclass
class Params:
    gpus: int = 4
    gpu_mibs: float = 0.0          # measured persisted-output rate of ONE GPU (MiB/s)
    gpu_hash_rate: float = 0.0     # alternative: raw node hashes/s of ONE GPU
    chunk_blocks: int = POSE_CHUNK_BLOCKS
    disk_sizes_bytes: list = field(default_factory=list)
    disk_mibs: float = 0.0         # per-disk sequential write cap; 0 = unlimited
    d2h_mibs: float = 0.0          # per-GPU device→host copy cap; 0 = unlimited
    host_mibs: float = 0.0         # aggregate host write cap (all disks); 0 = unlimited
    pool_eff: float = 1.0          # efficiency when K>1 GPUs share one disk (0..1]
    sector_bytes: int = 4096       # disk logical sector size (BLKSSZGET)
    gpus_per_disk: int = 0         # 0 = sweep all K; else also highlight this K


def resolve_gpu_rate(p: Params) -> tuple[float, str]:
    """Persisted-output bytes/s of one GPU and where the number came from."""
    if p.gpu_mibs > 0.0:
        return p.gpu_mibs * MIB, "measured (--gpu-mibs)"
    if p.gpu_hash_rate > 0.0:
        # Every persisted label costs excess(cb) node hashes; the derived rate is an
        # UPPER BOUND (assumes perfect occupancy, no level-sync stalls).
        bps = p.gpu_hash_rate * POSE_HASH_BYTES / excess_multiplier(p.chunk_blocks)
        return bps, "derived upper bound (--gpu-hash-rate / excess multiplier)"
    return 0.0, "none"


# ── scheduling model ───────────────────────────────────────────────────────────
def team_rate_bps(p: Params, gpu_bps: float, k: int) -> tuple[float, str]:
    """Bytes/s one team of k GPUs can push into one disk, and the binding stage."""
    eff = p.pool_eff if k > 1 else 1.0
    stages = {
        "gpu labeler": k * gpu_bps * eff,
        "d2h copy": k * p.d2h_mibs * MIB if p.d2h_mibs > 0 else math.inf,
        "disk write": p.disk_mibs * MIB if p.disk_mibs > 0 else math.inf,
    }
    stage = min(stages, key=stages.get)
    return stages[stage], stage


def lpt_schedule(sizes: list[float], teams: int, rate_bps: float) -> tuple[float, list[float]]:
    """Longest-processing-time-first assignment of disks to `teams` concurrent
    teams, each writing at rate_bps.  Returns (makespan_s, per-team busy seconds)."""
    busy = [0.0] * teams
    for sz in sorted(sizes, reverse=True):
        i = busy.index(min(busy))
        busy[i] += sz / rate_bps
    return max(busy), busy


def evaluate_k(p: Params, gpu_bps: float, k: int) -> dict:
    teams = p.gpus // k
    idle_gpus = p.gpus - teams * k
    rate, stage = team_rate_bps(p, gpu_bps, k)

    # Host aggregate cap: when every team is streaming, they share host_mibs.
    host_bound = False
    if p.host_mibs > 0.0 and teams * rate > p.host_mibs * MIB:
        rate = p.host_mibs * MIB / teams
        stage = "host aggregate write"
        host_bound = True

    # Sector padding: the bench rounds super_bytes up to the sector for O_DIRECT.
    super_bytes = p.chunk_blocks * POSE_HASH_BYTES
    aligned = math.ceil(super_bytes / p.sector_bytes) * p.sector_bytes
    pad_factor = aligned / super_bytes        # 1.0 for cb >= sector/32
    effective_rate = rate / pad_factor        # payload rate after padding overhead

    makespan, busy = lpt_schedule(p.disk_sizes_bytes, teams, effective_rate)
    total_bytes = sum(p.disk_sizes_bytes)
    ideal = total_bytes / (p.gpus * gpu_bps)  # perfect pooling, no caps
    return dict(
        k=k, teams=teams, idle_gpus=idle_gpus,
        team_rate_mibs=rate / MIB, binding_stage=stage, host_bound=host_bound,
        pad_factor=pad_factor,
        concurrent_disks=min(teams, len(p.disk_sizes_bytes)),
        makespan_s=makespan,
        team_busy_s=busy,
        gpu_utilisation=(ideal / makespan) if makespan > 0 else 0.0,
    )


def compute(p: Params) -> dict:
    gpu_bps, rate_source = resolve_gpu_rate(p)
    if gpu_bps <= 0.0:
        raise ValueError("no GPU rate — provide --gpu-mibs (measured) or --gpu-hash-rate")
    if not p.disk_sizes_bytes:
        raise ValueError("no disks — provide --disk-tb [--disks N] or --disk-sizes-tb a,b,c")
    if p.gpus < 1:
        raise ValueError("--gpus must be >= 1")

    total_bytes = sum(p.disk_sizes_bytes)
    cb = p.chunk_blocks
    excess = excess_multiplier(cb)
    sweep = [evaluate_k(p, gpu_bps, k) for k in range(1, p.gpus + 1)]
    best = min(sweep, key=lambda r: r["makespan_s"])
    by_k = {r["k"]: r for r in sweep}

    return dict(
        rate_source=rate_source,
        gpu_mibs=gpu_bps / MIB,
        implied_node_hashes_per_sec=gpu_bps / POSE_HASH_BYTES * excess,
        chunk_blocks=cb, valid_cb=is_valid_chunk_blocks(cb),
        excess=excess, scaffold_nodes=scratch_node_count(cb),
        super_bytes=cb * POSE_HASH_BYTES,
        total_bytes=total_bytes,
        aggregate_gpu_mibs=p.gpus * gpu_bps / MIB,
        ideal_s=total_bytes / (p.gpus * gpu_bps),
        single_gpu_s=total_bytes / gpu_bps,
        sweep=sweep, best=best,
        gpus_per_disk=by_k[p.gpus],       # strategy A: all GPUs → one disk at a time
        disk_per_gpu=by_k[1],             # strategy B: one GPU per disk, Y in flight
        chosen=by_k.get(p.gpus_per_disk) if p.gpus_per_disk else None,
    )


# ── formatting ─────────────────────────────────────────────────────────────────
def fmt_duration(s: float) -> str:
    if not math.isfinite(s):
        return "inf"
    if s < 1.0:
        return f"{s * 1e3:.1f} ms"
    if s < 60.0:
        return f"{s:.1f} seconds"
    if s < 3600.0:
        return f"{int(s // 60)} minutes {int(s % 60)} seconds"
    if s < 86400.0:
        return f"{int(s // 3600)} hours {int(s // 60) % 60} minutes"
    return f"{s / 86400.0:.1f} days"


def fmt_bytes(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if n < 1024 or unit == "TiB":
            return f"{n:.2f} {unit}" if unit != "B" else f"{int(n)} B"
        n /= 1024.0
    return f"{n:.2f} TiB"


def _strategy_line(name: str, r: dict) -> list[str]:
    L = [f"  {name}"]
    L.append(f"    GPUs per disk (K)    : {r['k']}   teams in flight: {r['teams']}"
             + (f"   idle GPUs: {r['idle_gpus']}" if r['idle_gpus'] else ""))
    L.append(f"    disks concurrently   : {r['concurrent_disks']}")
    L.append(f"    per-disk write rate  : {r['team_rate_mibs']:.1f} MiB/s  "
             f"(bound by {r['binding_stage']})")
    if r["pad_factor"] > 1.0:
        L.append(f"    sector padding       : {(1 - 1 / r['pad_factor']) * 100:.0f}% of each "
                 "write is padding (super-chunk smaller than a sector multiple)")
    L.append(f"    total wipe time      : {fmt_duration(r['makespan_s'])}  "
             f"({r['makespan_s']:.0f} s)")
    L.append(f"    GPU utilisation      : {r['gpu_utilisation'] * 100:.0f}%")
    return L


def format_report(p: Params, m: dict) -> str:
    L = []
    L.append("=" * 68)
    L.append("  GPU-driven disk wipe estimate (PoSE-DB, faithful labeler)")
    L.append("=" * 68)
    L.append(f"  GPU persisted rate   : {m['gpu_mibs']:.1f} MiB/s per GPU  [{m['rate_source']}]")
    L.append(f"  GPUs                 : {p.gpus}   (aggregate {m['aggregate_gpu_mibs']:.1f} MiB/s)")
    L.append(f"  chunk_blocks         : {p.chunk_blocks}"
             + ("" if m["valid_cb"] else "  [INVALID: need power-of-two <= 2^20]"))
    L.append(f"  excess-hash multiplier: {m['excess']:.0f}x  "
             f"({m['scaffold_nodes']} scaffold nodes / {p.chunk_blocks} persisted labels)")
    L.append(f"  implied node hashes  : {m['implied_node_hashes_per_sec']:.3g} H/s per GPU  "
             "(what the labeler is actually hashing)")
    L.append(f"  super-chunk (disk)   : {fmt_bytes(m['super_bytes'])} per pwrite unit")
    L.append("")
    L.append("-- Storage --------------------------------------------------------")
    sizes = p.disk_sizes_bytes
    L.append(f"  disks                : {len(sizes)}")
    L.append(f"  total                : {sum(sizes) / TB:.2f} TB  ({fmt_bytes(sum(sizes))})")
    if len(set(sizes)) == 1:
        L.append(f"  per disk             : {sizes[0] / TB:.2f} TB")
    else:
        L.append("  per disk (TB)        : " + ", ".join(f"{s / TB:.2f}" for s in sizes))
    caps = []
    caps.append(f"disk write {p.disk_mibs:.0f} MiB/s" if p.disk_mibs > 0 else "disk write unlimited")
    caps.append(f"D2H {p.d2h_mibs:.0f} MiB/s/GPU" if p.d2h_mibs > 0 else "D2H unlimited")
    caps.append(f"host aggregate {p.host_mibs:.0f} MiB/s" if p.host_mibs > 0 else "host aggregate unlimited")
    L.append(f"  caps                 : {'; '.join(caps)}")
    if p.pool_eff < 1.0:
        L.append(f"  pooling efficiency   : {p.pool_eff:.2f} when K > 1 GPUs share a disk")
    L.append("")
    L.append("-- Bounds ---------------------------------------------------------")
    L.append(f"  one GPU, serial      : {fmt_duration(m['single_gpu_s'])}")
    L.append(f"  ideal (perfect pool) : {fmt_duration(m['ideal_s'])}  "
             f"= total / ({p.gpus} x {m['gpu_mibs']:.1f} MiB/s)")
    L.append("")
    L.append("-- Strategies -----------------------------------------------------")
    L.extend(_strategy_line("A. gpus-per-disk  (all GPUs -> one disk at a time)", m["gpus_per_disk"]))
    L.append("")
    L.extend(_strategy_line("B. disk-per-gpu   (one GPU per disk, disks in parallel)", m["disk_per_gpu"]))
    if m["chosen"] is not None and m["chosen"]["k"] not in (1, p.gpus):
        L.append("")
        L.extend(_strategy_line(f"C. requested      (K = {m['chosen']['k']} GPUs per disk)", m["chosen"]))
    L.append("")
    L.append("-- Sweep: K GPUs per disk -----------------------------------------")
    L.append("    K  teams  idle  disk MiB/s  bound by               total        util")
    for r in m["sweep"]:
        mark = " <-- best" if r is m["best"] else ""
        L.append(f"  {r['k']:3d}  {r['teams']:5d}  {r['idle_gpus']:4d}  "
                 f"{r['team_rate_mibs']:10.1f}  {r['binding_stage']:<21s}  "
                 f"{fmt_duration(r['makespan_s']):<22s} {r['gpu_utilisation'] * 100:3.0f}%{mark}")
    L.append("")
    b = m["best"]
    L.append(f"  best: K = {b['k']} GPUs per disk, {b['teams']} disk(s) in flight -> "
             f"{fmt_duration(b['makespan_s'])}")
    L.append("=" * 68)
    return "\n".join(L)


def to_json(p: Params, m: dict) -> str:
    def strat(r: dict) -> dict:
        return dict(gpus_per_disk=r["k"], teams=r["teams"], idle_gpus=r["idle_gpus"],
                    concurrent_disks=r["concurrent_disks"],
                    per_disk_mib_per_sec=round(r["team_rate_mibs"], 3),
                    binding_stage=r["binding_stage"],
                    seconds=round(r["makespan_s"], 3),
                    human=fmt_duration(r["makespan_s"]),
                    gpu_utilisation=round(r["gpu_utilisation"], 4))
    out = dict(
        source=m["rate_source"],
        gpu=dict(count=p.gpus, persisted_mib_per_sec=round(m["gpu_mibs"], 3),
                 aggregate_mib_per_sec=round(m["aggregate_gpu_mibs"], 3),
                 chunk_blocks=p.chunk_blocks, excess_multiplier=round(m["excess"], 2),
                 implied_node_hashes_per_sec=round(m["implied_node_hashes_per_sec"])),
        storage=dict(disks=len(p.disk_sizes_bytes),
                     total_tb=round(m["total_bytes"] / TB, 4),
                     disk_sizes_tb=[round(s / TB, 4) for s in p.disk_sizes_bytes],
                     disk_mib_per_sec_cap=p.disk_mibs, d2h_mib_per_sec_cap=p.d2h_mibs,
                     host_mib_per_sec_cap=p.host_mibs, sector_bytes=p.sector_bytes),
        bounds=dict(single_gpu_seconds=round(m["single_gpu_s"], 3),
                    single_gpu_human=fmt_duration(m["single_gpu_s"]),
                    ideal_seconds=round(m["ideal_s"], 3),
                    ideal_human=fmt_duration(m["ideal_s"])),
        gpus_per_disk=strat(m["gpus_per_disk"]),
        disk_per_gpu=strat(m["disk_per_gpu"]),
        best=strat(m["best"]),
        sweep=[strat(r) for r in m["sweep"]],
    )
    if m["chosen"] is not None:
        out["requested"] = strat(m["chosen"])
    return json.dumps(out, indent=2)


# ── field table (single source of truth for CLI flags, prompts, and the web form) ─
# (key, flag, label, help, type, default)   type ∈ {"float","int","str"}
FIELDS = [
    ("gpu_mibs", "--gpu-mibs", "GPU persisted rate (MiB/s)",
     "Measured output rate of ONE GPU — the 'GPU wipe: … = X MiB/s' line, "
     "at the same chunk_blocks the disk sessions will use. Leave 0 if using a raw hash rate.",
     "float", 0.0),
    ("gpu_hash_rate", "--gpu-hash-rate", "GPU raw hash rate (H/s, optional)",
     "Only if you have no measured rate: raw node hashes/s of one GPU; gives an optimistic "
     "upper bound via the scaffold excess multiplier.", "float", 0.0),
    ("gpus", "--gpus", "Number of GPUs", "How many of this GPU are available.", "int", 4),
    ("chunk_blocks", "--chunk-blocks", "chunk_blocks",
     "Session graph size (power of two, 1..2^20). Use the value the rate was measured at: "
     "the disk-wipe-bench runs in this repository use 131072. Informational only for the "
     "time estimate.", "int", POSE_CHUNK_BLOCKS),
    ("disks", "--disks", "Number of disks", "Equal-size disks (ignored if per-disk sizes given).",
     "int", 1),
    ("disk_tb", "--disk-tb", "Total capacity (TB, decimal)",
     "Total disk capacity to wipe, in vendor TB (1e12 bytes). Split equally across disks.",
     "float", 0.0),
    ("disk_sizes_tb", "--disk-sizes-tb", "Per-disk sizes (TB, comma-separated, optional)",
     "e.g. 7.68,7.68,15.36 — overrides total/number of disks.", "str", ""),
    ("disk_mibs", "--disk-mibs", "Per-disk sustained write cap (MiB/s, 0 = none)",
     "Sustained sequential write of one drive (after SLC cache exhaustion), not datasheet burst.",
     "float", 0.0),
    ("d2h_mibs", "--d2h-mibs", "Device→host copy cap per GPU (MiB/s, 0 = none)",
     "PCIe device-to-host bandwidth per GPU (Gen5 x16 ≈ 50000).", "float", 0.0),
    ("host_mibs", "--host-mibs", "Aggregate host write cap (MiB/s, 0 = none)",
     "Backplane / total NVMe write bandwidth shared by all disks.", "float", 0.0),
    ("pool_eff", "--pool-eff", "Pooling efficiency (0–1]",
     "Efficiency when K > 1 GPUs feed one disk (1.0 = perfect split at super-chunk granularity).",
     "float", 1.0),
    ("sector_bytes", "--sector-bytes", "Disk sector size (bytes)",
     "Logical sector size for O_DIRECT rounding (usually 4096).", "int", 4096),
    ("gpus_per_disk", "--gpus-per-disk", "Also report this K GPUs-per-disk (0 = skip)",
     "K=1 and K=gpus are always reported; add one more split to highlight.", "int", 0),
]
_FIELD = {f[0]: f for f in FIELDS}


def _coerce(key: str, raw) -> object:
    typ = _FIELD[key][4]
    if raw is None or (isinstance(raw, str) and raw.strip() == ""):
        return _FIELD[key][5]
    if typ == "float":
        return float(raw)
    if typ == "int":
        return int(float(raw))
    return str(raw).strip()


def build_params(values: dict) -> Params:
    """Build Params from a {key: raw value} dict (CLI, prompt, or web form).
    Raises ValueError with a user-facing message on bad input."""
    v = {k: _coerce(k, values.get(k)) for k in _FIELD}
    if v["disk_sizes_tb"]:
        sizes = [float(x) * TB for x in v["disk_sizes_tb"].split(",") if x.strip()]
    elif v["disk_tb"] > 0.0 and v["disks"] > 0:
        sizes = [v["disk_tb"] * TB / v["disks"]] * v["disks"]
    else:
        sizes = []
    if any(sz <= 0 for sz in sizes):
        raise ValueError("disk sizes must be positive")
    if v["gpus"] < 1:
        raise ValueError("number of GPUs must be >= 1")
    if v["gpus_per_disk"] and not (1 <= v["gpus_per_disk"] <= v["gpus"]):
        raise ValueError("GPUs-per-disk must be between 1 and the number of GPUs")
    if not (0.0 < v["pool_eff"] <= 1.0):
        raise ValueError("pooling efficiency must be in (0, 1]")
    if v["sector_bytes"] < POSE_HASH_BYTES:
        raise ValueError("sector size must be >= 32 bytes")
    return Params(gpus=v["gpus"], gpu_mibs=v["gpu_mibs"], gpu_hash_rate=v["gpu_hash_rate"],
                  chunk_blocks=v["chunk_blocks"], disk_sizes_bytes=sizes,
                  disk_mibs=v["disk_mibs"], d2h_mibs=v["d2h_mibs"], host_mibs=v["host_mibs"],
                  pool_eff=v["pool_eff"], sector_bytes=v["sector_bytes"],
                  gpus_per_disk=v["gpus_per_disk"])


# ── interactive prompts ────────────────────────────────────────────────────────
def run_interactive() -> int:
    """Prompt for every field (Enter keeps the default), print the report, and offer
    to change individual values until the user quits."""
    print("disk-calc — GPU-driven disk wipe estimate.  Enter keeps the value in [brackets].\n")
    values = {k: f[5] for k, f in _FIELD.items()}
    # Sensible starting point for the two fields a first-time user must fill.
    order = [f[0] for f in FIELDS]

    def ask(key):
        _, _, label, help_, typ, _ = _FIELD[key]
        cur = values[key]
        shown = cur if cur not in ("", None) else "none"
        while True:
            raw = input(f"{label} [{shown}]: ").strip()
            if raw in ("?", "h", "help"):
                print(f"   {help_}")
                continue
            if raw == "":
                return
            try:
                values[key] = _coerce(key, raw)
                return
            except ValueError:
                print(f"   expected a {typ}; try again (or '?' for help)")

    for k in order:
        ask(k)

    while True:
        try:
            m = compute(build_params(values))
        except ValueError as e:
            print(f"\n!! {e}\n")
        else:
            print()
            print(format_report(build_params(values), m))
            print("\nFlags for this run:\n  " + flags_for(values) + "\n")
        print("Change a value?  number to edit, 'a' to re-enter all, 'q' to quit")
        for i, k in enumerate(order, 1):
            print(f"  {i:2d}. {_FIELD[k][2]} = {values[k]}")
        choice = input("> ").strip().lower()
        if choice in ("q", "quit", "exit", ""):
            return 0
        if choice == "a":
            for k in order:
                ask(k)
            continue
        if choice.isdigit() and 1 <= int(choice) <= len(order):
            ask(order[int(choice) - 1])


def flags_for(values: dict) -> str:
    """Equivalent non-interactive command line for the given values."""
    parts = ["python3 tools/disk-calc/disk_calc.py"]
    for key, flag, _, _, _, default in FIELDS:
        val = values.get(key, default)
        if val != default and val not in ("", None):
            parts.append(f"{flag} {val}")
    return " ".join(parts)


# ── local web UI (stdlib http.server) ──────────────────────────────────────────
_PAGE = r"""<!doctype html>
<html><head><meta charset="utf-8"><title>disk-calc</title>
<style>
  :root { color-scheme: light dark; --bd:#8884; --acc:#2a7ae2; }
  body { font: 14px/1.4 system-ui, sans-serif; margin: 0; display: grid;
         grid-template-columns: minmax(300px, 380px) 1fr; min-height: 100vh; }
  form { padding: 18px 20px; border-right: 1px solid var(--bd); }
  h1 { font-size: 17px; margin: 0 0 4px; } .sub { opacity:.7; margin: 0 0 14px; }
  label { display: block; margin: 12px 0 0; font-weight: 600; }
  small { display: block; opacity: .7; font-weight: 400; margin: 2px 0 4px; }
  input { width: 100%; box-sizing: border-box; padding: 6px 8px; font: inherit;
          border: 1px solid var(--bd); border-radius: 6px; background: transparent; color: inherit; }
  input:focus { outline: 2px solid var(--acc); }
  fieldset { border: 1px solid var(--bd); border-radius: 8px; margin: 14px 0 0; padding: 0 12px 12px; }
  legend { font-weight: 700; padding: 0 6px; }
  main { padding: 18px 22px; overflow: auto; }
  .cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(210px, 1fr)); gap: 12px; margin-bottom: 16px; }
  .card { border: 1px solid var(--bd); border-radius: 10px; padding: 12px 14px; }
  .card h3 { margin: 0 0 6px; font-size: 13px; opacity: .75; font-weight: 600; }
  .card .big { font-size: 22px; font-weight: 700; } .card .meta { opacity: .75; font-size: 12px; }
  .best { border-color: var(--acc); box-shadow: 0 0 0 1px var(--acc) inset; }
  table { border-collapse: collapse; width: 100%; margin: 6px 0 16px; }
  th, td { text-align: right; padding: 5px 8px; border-bottom: 1px solid var(--bd); }
  th:first-child, td:first-child, td.l, th.l { text-align: left; }
  tr.best td { font-weight: 700; color: var(--acc); }
  pre { font: 12px/1.35 ui-monospace, Menlo, monospace; border: 1px solid var(--bd);
        border-radius: 8px; padding: 12px; overflow-x: auto; }
  .err { color: #c33; font-weight: 600; margin: 8px 0; }
  .cmd { font: 12px ui-monospace, monospace; opacity: .8; word-break: break-all; }
  @media (max-width: 760px) { body { grid-template-columns: 1fr; } form { border-right: 0; border-bottom: 1px solid var(--bd); } }
</style></head>
<body>
<form id="f" onsubmit="return false">
  <h1>disk-calc</h1>
  <p class="sub">GPU-driven PoSE-DB disk wipe time. Results update as you type.</p>
  %FIELDS%
</form>
<main id="out"><p class="sub">Enter a GPU rate and a disk size to begin.</p></main>
<script>
const f = document.getElementById('f'), out = document.getElementById('out');
let t;
function fmt(n, d=1) { return Number(n).toLocaleString(undefined, {maximumFractionDigits: d}); }
function card(title, r, best) {
  return `<div class="card ${best ? 'best' : ''}"><h3>${title}</h3>
    <div class="big">${r.human}</div>
    <div class="meta">K=${r.gpus_per_disk} GPU/disk · ${r.concurrent_disks} disk(s) in flight ·
    ${fmt(r.per_disk_mib_per_sec)} MiB/s per disk · bound by ${r.binding_stage} ·
    ${Math.round(r.gpu_utilisation*100)}% GPU util</div></div>`;
}
function render(j) {
  if (j.error) { out.innerHTML = `<p class="err">${j.error}</p>`; return; }
  const bestK = j.best.gpus_per_disk;
  let rows = j.sweep.map(r => `<tr class="${r.gpus_per_disk===bestK?'best':''}">
    <td>${r.gpus_per_disk}</td><td>${r.teams}</td><td>${r.idle_gpus}</td>
    <td>${fmt(r.per_disk_mib_per_sec)}</td><td class="l">${r.binding_stage}</td>
    <td class="l">${r.human}</td><td>${Math.round(r.gpu_utilisation*100)}%</td></tr>`).join('');
  out.innerHTML = `
    <div class="cards">
      ${card('A · all GPUs → one disk at a time', j.gpus_per_disk, bestK===j.gpus_per_disk.gpus_per_disk)}
      ${card('B · one GPU per disk, disks in parallel', j.disk_per_gpu, bestK===1)}
      ${j.requested ? card('C · requested split', j.requested, bestK===j.requested.gpus_per_disk) : ''}
      <div class="card"><h3>Bounds</h3><div class="meta">one GPU, serial: <b>${j.bounds.single_gpu_human}</b><br>
        perfect pooling of ${j.gpu.count}: <b>${j.bounds.ideal_human}</b><br>
        excess-hash multiplier ${fmt(j.gpu.excess_multiplier,0)}× at cb=${j.gpu.chunk_blocks}<br>
        total ${fmt(j.storage.total_tb,2)} TB over ${j.storage.disks} disk(s)</div></div>
    </div>
    <h3>Sweep — K GPUs per disk</h3>
    <table><tr><th>K</th><th>teams</th><th>idle</th><th>disk MiB/s</th><th class="l">bound by</th><th class="l">total</th><th>util</th></tr>${rows}</table>
    <h3>Text report</h3><pre>${j.report.replace(/</g,'&lt;')}</pre>
    <p class="cmd">${j.command}</p>`;
}
async function calc() {
  const q = new URLSearchParams(new FormData(f)).toString();
  try { render(await (await fetch('/calc?' + q)).json()); }
  catch (e) { out.innerHTML = `<p class="err">${e}</p>`; }
}
f.addEventListener('input', () => { clearTimeout(t); t = setTimeout(calc, 200); });
calc();
</script></body></html>
"""

_GROUPS = [("GPU", ["gpu_mibs", "gpu_hash_rate", "gpus", "chunk_blocks"]),
           ("Storage", ["disks", "disk_tb", "disk_sizes_tb", "sector_bytes"]),
           ("Throughput caps (0 = unlimited)", ["disk_mibs", "d2h_mibs", "host_mibs", "pool_eff"]),
           ("Extra", ["gpus_per_disk"])]


def _render_page() -> str:
    import html
    parts = []
    for title, keys in _GROUPS:
        parts.append(f"<fieldset><legend>{html.escape(title)}</legend>")
        for k in keys:
            _, _, label, help_, typ, default = _FIELD[k]
            itype = "text" if typ == "str" else "number"
            step = ' step="any"' if typ == "float" else ""
            parts.append(f'<label>{html.escape(label)}<small>{html.escape(help_)}</small>'
                         f'<input type="{itype}" name="{k}" value="{html.escape(str(default))}"{step}></label>')
        parts.append("</fieldset>")
    return _PAGE.replace("%FIELDS%", "\n".join(parts))


def serve(port: int) -> int:
    import webbrowser
    from http.server import BaseHTTPRequestHandler, HTTPServer
    from urllib.parse import parse_qs, urlparse

    page = _render_page().encode()

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def _send(self, code, body, ctype):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            u = urlparse(self.path)
            if u.path == "/":
                return self._send(200, page, "text/html; charset=utf-8")
            if u.path == "/calc":
                q = {k: v[0] for k, v in parse_qs(u.query).items()}
                try:
                    p = build_params(q)
                    m = compute(p)
                    out = json.loads(to_json(p, m))
                    out["report"] = format_report(p, m)
                    out["command"] = flags_for({k: _coerce(k, q.get(k)) for k in _FIELD})
                except (ValueError, ZeroDivisionError) as e:
                    out = {"error": str(e)}
                return self._send(200, json.dumps(out).encode(), "application/json")
            self._send(404, b"not found", "text/plain")

    srv = HTTPServer(("127.0.0.1", port), H)
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    print(f"disk-calc UI at {url}  (Ctrl-C to stop)")
    webbrowser.open(url)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


# ── CLI ────────────────────────────────────────────────────────────────────────
def parse_args(argv=None):
    ap = argparse.ArgumentParser(
        description="Estimate GPU-driven PoSE-DB disk wipe time for X TB across Y GPUs.",
        epilog="No flags to remember: run with --interactive for prompts, or --serve for a "
               "local web form. Feed the PERSISTED rate the labeler prints "
               "('GPU wipe: ... = X MiB/s'), not a raw hash rate.")
    ap.add_argument("-i", "--interactive", action="store_true",
                    help="prompt for each value (Enter keeps the default)")
    ap.add_argument("--serve", nargs="?", const=8765, type=int, metavar="PORT",
                    help="open a local web form (default port 8765; 0 = any free port)")
    ap.add_argument("--json", action="store_true", help="emit JSON instead of the text report")
    for key, flag, label, help_, typ, default in FIELDS:
        kind = {"float": float, "int": int, "str": str}[typ]
        ap.add_argument(flag, dest=key, type=kind, default=default,
                        help=f"{label}. {help_} (default {default!r})")
    return ap.parse_args(argv)


def main(argv=None) -> int:
    a = parse_args(argv)
    if a.interactive:
        return run_interactive()
    if a.serve is not None:
        return serve(a.serve)
    values = {k: getattr(a, k) for k in _FIELD}
    try:
        p = build_params(values)
        m = compute(p)
    except ValueError as e:
        print(f"disk-calc: {e}", file=sys.stderr)
        print("hint: run with --interactive or --serve to be prompted for each value",
              file=sys.stderr)
        return 1
    print(to_json(p, m) if a.json else format_report(p, m))
    return 0


if __name__ == "__main__":
    sys.exit(main())
