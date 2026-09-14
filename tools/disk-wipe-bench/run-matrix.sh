#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd
#
# run-matrix.sh — run disk-wipe-bench across a fixed set of GPU -> drive
# configurations unattended (e.g. overnight), keeping one full log per run
# and one summary CSV row per run.
#
#   ⚠ DESTRUCTIVE.  Every configuration is a REAL wipe of the listed drives.
#     The bench's own guards still apply (explicit --disk only, refuses
#     mounted / held devices, O_EXCL), but this script passes --confirm, so
#     anything it is pointed at that passes those guards WILL be overwritten.
#     Check SLOW_DISK_A / SLOW_DISK_B / FAST_DISK below before running.
#
# Usage (as root, from anywhere; detach so it survives the SSH session):
#
#   sudo FAST_DISK=/dev/nvmeXn1 nohup tools/disk-wipe-bench/run-matrix.sh > /dev/null 2>&1 &
#   tail -f tools/disk-wipe-bench/logs/<timestamp>/matrix.log
#
# or inside tmux/screen without nohup.  All knobs are environment variables:
#
#   BIN           bench binary            (default: build/disk-wipe-bench next to this script)
#   SLOW_DISK_A   first  1.46 TiB drive   (default /dev/nvme0n1)
#   SLOW_DISK_B   second 1.46 TiB drive   (default /dev/nvme1n1)
#   FAST_DISK     the 1.92 TB / 4000 MiB/s drive (no default — REQUIRED for all single-drive configs; skipped if unset)
#   CHUNK_BLOCKS  graph size              (default 131072)
#   VERIFY        super-chunks to sample-verify after each wipe (default 16; 0 = skip).
#                 Each sample is one full CPU graph recompute (~tens of seconds at cb=2^17).
#   COOLDOWN      seconds to idle between runs (default 60)
#   ONLY          space-separated list of config names to run (default: all)
#   GPU_POOL      ordinals for the scaling sweep, first N are used (default "0 1 2 3 4 5 6 7")
#   SWEEP_MAX     largest N in the 1..N GPU sweep against the fast drive (default 8)
#   DRY_RUN=1     pass --dry-run instead of --confirm (label + D2H only, no writes,
#                 verify disabled) — use this first to check devices and flags
#   IDLE_WAIT     seconds to wait for other compute processes (e.g. an inference server) to leave the
#                 GPUs a run needs before giving up and skipping it (default 3600; 0 = never
#                 wait, skip immediately). A shared GPU produces a meaningless number.
#   SAMPLE_EVERY  seconds between in-run nvidia-smi samples (default 10; 0 = off)
#   LOG_ROOT      where to put logs      (default: logs/ next to this script)
#
# Output:
#   $LOG_ROOT/<YYYYmmdd-HHMMSS>/matrix.log          driver log (this script)
#   $LOG_ROOT/<YYYYmmdd-HHMMSS>/NN-<name>.log       full bench output per run
#   $LOG_ROOT/<YYYYmmdd-HHMMSS>/NN-<name>-gpu.csv   in-run GPU samples: clock, power,
#                                                    temperature, throttle reasons, HBM used
#   $LOG_ROOT/<YYYYmmdd-HHMMSS>/summary.csv         one row per run
#
# The bench detects that stdout is not a TTY and prints compact progress lines
# instead of the redraw dashboard, so the per-run logs are clean.

set -u
set -o pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="${BIN:-$HERE/build/disk-wipe-bench}"
SLOW_DISK_A="${SLOW_DISK_A:-/dev/nvme0n1}"
SLOW_DISK_B="${SLOW_DISK_B:-/dev/nvme1n1}"
FAST_DISK="${FAST_DISK:-}"
CHUNK_BLOCKS="${CHUNK_BLOCKS:-131072}"
VERIFY="${VERIFY:-16}"
COOLDOWN="${COOLDOWN:-60}"
ONLY="${ONLY:-}"
DRY_RUN="${DRY_RUN:-0}"
IDLE_WAIT="${IDLE_WAIT:-3600}"
SAMPLE_EVERY="${SAMPLE_EVERY:-10}"
LOG_ROOT="${LOG_ROOT:-$HERE/logs}"

# ── configuration matrix ──────────────────────────────────────────────────────
# name | --gpus list | scheduling flag(s) | disks (space-separated; FAST expands to $FAST_DISK)
#
# Part 1 — GPU scaling sweep: N GPUs -> the ONE fast drive, N = 1..SWEEP_MAX,
# taking the first N ordinals of GPU_POOL.  Single-drive runs always use the
# fast drive so the GPUs, not the medium, are what is being measured.
# Part 2 — two-drives-in-parallel schedules on the two slow drives (there is only
# one fast one).  Sequential all-GPUs-on-one-slow-drive runs are covered by the sweep.
# Runs go in list order.
GPU_POOL="${GPU_POOL:-0 1 2 3 4 5 6 7}"   # ordinals to draw from (drop any GPU you want left alone)
SWEEP_MAX="${SWEEP_MAX:-8}"

CONFIGS=()
read -r -a _pool <<< "$GPU_POOL"
for ((N=1; N<=SWEEP_MAX && N<=${#_pool[@]}; N++)); do
  _gpus="$(IFS=,; echo "${_pool[*]:0:N}")"
  CONFIGS+=("$(printf '%dgpu-1fast-%dper | %s | --mode gpus-per-disk | FAST' "$N" "$N" "$_gpus")")
done
CONFIGS+=(
  "4gpu-2slow-2per   | 0,1,2,3         | --gpus-per-disk 2     | $SLOW_DISK_A $SLOW_DISK_B"
  "6gpu-2slow-3per   | 0,1,2,3,4,5     | --gpus-per-disk 3     | $SLOW_DISK_A $SLOW_DISK_B"
)

# ── setup ─────────────────────────────────────────────────────────────────────
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG_DIR="$LOG_ROOT/$STAMP"
mkdir -p "$LOG_DIR"
MATRIX_LOG="$LOG_DIR/matrix.log"
SUMMARY="$LOG_DIR/summary.csv"

log() { printf '%s  %s\n' "$(date '+%F %T')" "$*" | tee -a "$MATRIX_LOG"; }

if [[ $EUID -ne 0 ]]; then
  echo "run-matrix.sh: must run as root (O_DIRECT on raw block devices)" >&2
  exit 1
fi
if [[ ! -x "$BIN" ]]; then
  echo "run-matrix.sh: bench binary not found or not executable: $BIN" >&2
  exit 1
fi

GIT_REV="$(git -C "$HERE" rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY="$(git -C "$HERE" status --porcelain 2>/dev/null | grep -q . && echo '+dirty' || true)"

log "=== disk-wipe-bench matrix ==="
log "binary       : $BIN ($(stat -c %y "$BIN" 2>/dev/null | cut -d. -f1))"
log "git          : ${GIT_REV}${GIT_DIRTY}"
log "chunk_blocks : $CHUNK_BLOCKS   verify: $VERIFY   cooldown: ${COOLDOWN}s   mode: $([[ $DRY_RUN == 1 ]] && echo DRY-RUN || echo 'REAL WRITES')"
log "disks        : slow A=$SLOW_DISK_A  slow B=$SLOW_DISK_B  fast=${FAST_DISK:-<unset, fast configs skipped>}"
log "log dir      : $LOG_DIR"
nvidia-smi --query-gpu=index,name,clocks.max.sm,memory.total --format=csv 2>/dev/null | tee -a "$MATRIX_LOG" || true
for d in "$SLOW_DISK_A" "$SLOW_DISK_B" ${FAST_DISK:+"$FAST_DISK"}; do
  [[ -b "$d" ]] && log "  $d  $(lsblk -dno SIZE,MODEL "$d" 2>/dev/null | tr -s ' ')" || log "  $d  (not a block device)"
done

echo "run,name,gpus,schedule,disks,total_written,wall_s,aggregate_mibs,label_pct,d2h_pct,write_pct,verify,exit_code,log" > "$SUMMARY"

# Other compute processes on the given GPUs (comma list of ordinals), one per line
# as "gpu_uuid, pid, name, used_mib"; empty when the GPUs are ours alone.
foreign_procs() {  # foreign_procs <gpus>
  nvidia-smi -i "$1" --query-compute-apps=gpu_uuid,pid,process_name,used_memory \
             --format=csv,noheader 2>/dev/null | grep -v '^\s*$' || true
}

# Extract one field from a finished log; empty string if absent.
field() {  # field <log> <sed-regex-with-one-group>
  sed -nE "s#$2#\1#p" "$1" | tail -1
}

# ── main loop ─────────────────────────────────────────────────────────────────
n=0
for entry in "${CONFIGS[@]}"; do
  IFS='|' read -r name gpus sched disks <<< "$entry"
  name="$(echo "$name" | xargs)"; gpus="$(echo "$gpus" | xargs)"
  sched="$(echo "$sched" | xargs)"; disks="$(echo "$disks" | xargs)"

  if [[ -n "$ONLY" ]] && ! grep -qw -- "$name" <<< "$ONLY"; then
    continue
  fi
  n=$((n+1))
  run_log="$LOG_DIR/$(printf '%02d' "$n")-$name.log"

  if [[ "$disks" == *FAST* ]]; then
    if [[ -z "$FAST_DISK" ]]; then
      log "[$n] $name: SKIPPED (FAST_DISK not set)"
      echo "$n,$name,$gpus,\"$sched\",FAST,,,,,,,skipped,,\"$run_log\"" >> "$SUMMARY"
      continue
    fi
    disks="${disks//FAST/$FAST_DISK}"
  fi

  disk_args=()
  for d in $disks; do disk_args+=(--disk "$d"); done
  write_args=(--confirm)
  verify_args=()
  if [[ "$DRY_RUN" == 1 ]]; then
    write_args=(--dry-run)
  elif [[ "$VERIFY" -gt 0 ]]; then
    verify_args=(--verify "$VERIFY")
  fi

  # shellcheck disable=SC2206
  cmd=("$BIN" "${disk_args[@]}" --gpus "$gpus" $sched --chunk-blocks "$CHUNK_BLOCKS" "${write_args[@]}" "${verify_args[@]}")

  # Refuse to share a GPU: anything else resident (an inference server, a notebook, another
  # bench) takes HBM from the warp scratch pool and SM time from the labeler,
  # and the result measures nothing.  Wait up to IDLE_WAIT for it to leave.
  waited=0
  while procs="$(foreign_procs "$gpus")"; [[ -n "$procs" ]]; do
    if (( waited == 0 )); then
      log "[$n] $name: other processes on GPUs $gpus — waiting up to ${IDLE_WAIT}s:"
      sed 's/^/        /' <<< "$procs" | tee -a "$MATRIX_LOG"
    fi
    if (( waited >= IDLE_WAIT )); then break; fi
    sleep 30; waited=$((waited+30))
  done
  if [[ -n "$procs" ]]; then
    log "[$n] $name: SKIPPED (GPUs still busy after ${waited}s)"
    echo "$n,$name,$gpus,\"$sched\",\"$disks\",,,,,,,gpus-busy,,\"$run_log\"" >> "$SUMMARY"
    continue
  fi
  (( waited > 0 )) && log "[$n] $name: GPUs free after ${waited}s"

  log "[$n] $name: START  gpus=$gpus  $sched  disks=$disks"
  log "[$n] cmd: ${cmd[*]}"
  {
    echo "# run-matrix: $name"
    echo "# started: $(date '+%F %T')"
    echo "# cmd: ${cmd[*]}"
    echo "# git: ${GIT_REV}${GIT_DIRTY}"
    nvidia-smi --query-gpu=index,clocks.sm,power.draw,temperature.gpu --format=csv 2>/dev/null || true
    echo
  } > "$run_log"

  # In-run GPU sampler: one CSV row per GPU every SAMPLE_EVERY seconds.  The
  # throttle-reasons bitmask names the cause if a GPU sits below boost
  # (0x4 = SW power cap, 0x20 = HW thermal slowdown, 0x40 = HW power brake).
  sampler_pid=""
  if (( SAMPLE_EVERY > 0 )); then
    gpu_csv="${run_log%.log}-gpu.csv"
    nvidia-smi -i "$gpus" -l "$SAMPLE_EVERY" \
      --query-gpu=timestamp,index,clocks.sm,power.draw,temperature.gpu,clocks_throttle_reasons.active,memory.used,utilization.gpu \
      --format=csv > "$gpu_csv" 2>/dev/null &
    sampler_pid=$!
  fi

  t0=$(date +%s)
  "${cmd[@]}" >> "$run_log" 2>&1
  rc=$?
  t1=$(date +%s)
  if [[ -n "$sampler_pid" ]]; then kill "$sampler_pid" 2>/dev/null; wait "$sampler_pid" 2>/dev/null; fi
  echo "# finished: $(date '+%F %T')  exit=$rc  elapsed=$((t1-t0))s" >> "$run_log"

  total="$(field "$run_log" '^total written *: *(.*)$')"
  wall="$(field "$run_log" '^wall clock *: .*\(([0-9.]+) s\)$')"
  agg="$(field "$run_log" '^aggregate *: *([0-9.]+) MiB/s.*$')"
  lab="$(field "$run_log" '^time split *: *label ([0-9]+)%.*$')"
  d2h="$(field "$run_log" '^time split *:.*d2h ([0-9]+)%.*$')"
  wr="$(field "$run_log" '^time split *:.*write ([0-9]+)%.*$')"
  ver="$(field "$run_log" '^.*: *([0-9]+/[0-9]+) sampled super-chunks match$')"
  [[ -z "$ver" ]] && { grep -qi mismatch "$run_log" && ver="MISMATCH"; }
  [[ -z "$ver" && ( "$VERIFY" -eq 0 || "$DRY_RUN" == 1 ) ]] && ver="not-run"

  echo "$n,$name,$gpus,\"$sched\",\"$disks\",\"$total\",$wall,$agg,$lab,$d2h,$wr,$ver,$rc,\"$run_log\"" >> "$SUMMARY"

  if [[ $rc -eq 0 ]]; then
    log "[$n] $name: DONE   ${total} in ${wall}s = ${agg} MiB/s  (label ${lab}% write ${wr}%)  verify=${ver:-n/a}"
    if (( SAMPLE_EVERY > 0 )) && [[ -s "$gpu_csv" ]]; then
      # Per-GPU mean SM clock over the run, so a throttled or shared GPU is visible at a glance.
      awk -F', *' 'NR>1 && $3 ~ /MHz/ {sub(/ MHz/,"",$3); s[$2]+=$3; c[$2]++}
                   END {for (g in s) printf "        gpu %s: mean SM clock %d MHz over %d samples\n", g, s[g]/c[g], c[g]}' \
          "$gpu_csv" | sort -k2 -n | tee -a "$MATRIX_LOG"
    fi
  else
    log "[$n] $name: FAILED exit=$rc — see $run_log"
    tail -5 "$run_log" | sed 's/^/        /' | tee -a "$MATRIX_LOG"
  fi
  [[ "$ver" == "MISMATCH" ]] && log "[$n] $name: *** VERIFY MISMATCH — labels are not byte-identical to the CPU path ***"

  sync
  if [[ $COOLDOWN -gt 0 ]]; then
    log "[$n] cooldown ${COOLDOWN}s"
    sleep "$COOLDOWN"
  fi
done

log "=== matrix complete: $n run(s) ==="
log "summary: $SUMMARY"
column -s, -t < "$SUMMARY" 2>/dev/null | cut -d'"' -f1-99 | tee -a "$MATRIX_LOG" || cat "$SUMMARY" | tee -a "$MATRIX_LOG"
