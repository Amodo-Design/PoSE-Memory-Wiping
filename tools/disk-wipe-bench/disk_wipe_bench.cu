/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Amodo Design Ltd */

/*
 * disk-wipe-bench — GPU-driven PoSE-DB disk wipe + throughput benchmark.
 *
 * Performs the REAL disk wipe: it fills every addressable sector of the chosen
 * block device(s) with PoSE-DB graph labels generated on the GPU, exactly as a
 * production wipe would, and reports the achieved throughput.  The labeling is
 * the erasure — writing the graph's output-set labels overwrites all prior disk
 * contents, and (being the challengeable output set O(G)) simultaneously proves
 * it.  This tool stops short of the challenge/proof phase; it measures how fast
 * the wipe itself runs so the parallel-wipe design can be sized on real numbers.
 *
 * It does NOT reimplement the algorithm.  Labels come from the validated
 * pose_graph_label_hbm (common/src/cuda) — byte-identical to the CPU in-place
 * path (pose_graph_label_region_pooled_inplace), so what this tool writes is
 * what the verifier would later challenge.  The only
 * new code is the host-side plumbing: GPU→host copy, O_DIRECT sequential writes,
 * multi-GPU scheduling, and safety guards.
 *
 * Pipeline per GPU worker (scaffold topology built ONCE per worker and reused
 * across every HBM window and every disk range it is assigned):
 *     pose_graph_label_hbm (fill an HBM window)  →  cudaMemcpy D2H (pinned)
 *       →  pwrite O_DIRECT (sequential, sector-aligned)  →  advance
 *
 * Scheduling (matches the disk-calc model): choose the GPU pool with --gpus,
 * then K GPUs team up per disk (--gpus-per-disk K), giving floor(pool/K) disks
 * wiped in parallel; within a team the disk's range is split into K disjoint
 * contiguous sector sub-ranges.  Two named --mode aliases cover the extremes:
 *   --mode disk-per-gpu    K=1: one GPU per disk, disks in parallel   (default)
 *   --mode gpus-per-disk   K=pool: all chosen GPUs cooperate on one disk at a
 *                          time, disks wiped sequentially
 *   --gpus-per-disk K      any K in between, e.g. pool of 4, K=2 -> 2 disks in
 *                          flight, 2 GPUs each
 *
 * SAFETY (non-negotiable — this destroys data on the target devices):
 *   - Only devices named explicitly with --disk are ever touched.  No scanning,
 *     no globbing, no auto-selection.
 *   - Any device that is mounted, or that carries a mounted partition, or that
 *     backs the root filesystem (/), is refused.  Real writes additionally open
 *     the device O_EXCL, so the kernel itself refuses anything held by a mounted
 *     filesystem, device-mapper (LVM/LUKS), md RAID, or swap — the cases a
 *     /proc/mounts scan cannot see.  Targets are realpath()'d first so a
 *     /dev/disk/by-id symlink cannot bypass either check.
 *   - The exact device list and byte ranges are printed before any write.
 *   - --dry-run does everything EXCEPT the pwrite (labels + D2H only): safe on
 *     any CUDA box, and the way to validate the pipeline before a real wipe.
 *
 * Requires a CUDA build (nvcc) and, for real writes, root (O_DIRECT on a raw
 * block device).  Runs on H100/H200 (or any CUDA GPU); throughput is
 * arch-specific so benchmark on the target.  See tools/disk-wipe-bench/README.md.
 */

#include "../../common/include/gpu_label.h"
#include "../../common/include/graph.h"
#include "../../common/include/hash.h"

#include <cuda_runtime.h>

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <initializer_list>

#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>   /* BLKGETSIZE64, BLKSSZGET */
#include <time.h>

static const uint64_t MIB = 1024ull * 1024ull;

/* Fixed benchmark seed — timing is seed-independent; this is not a verifier
 * secret.  Overridable with --seed-hex for a specific reproducible label set. */
static uint8_t g_seed[POSE_HASH_BYTES] = {
    0x44, 0x69, 0x73, 0x6b, 0x57, 0x69, 0x70, 0x65,   /* "DiskWipe" */
    0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    0xfe, 0xed, 0xfa, 0xce, 0x0b, 0xad, 0xf0, 0x0d,
    0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
};

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static std::mutex g_print_mu;   /* serialize interleaved worker prints */

/* ── config ─────────────────────────────────────────────────────────────── */
struct Config {
    std::vector<std::string> disks;      /* explicit device paths */
    std::vector<int>         gpus;       /* CUDA device ordinals to use */
    uint32_t chunk_blocks = POSE_CHUNK_BLOCKS;
    uint64_t per_disk_bytes = 0;         /* 0 = whole device */
    uint64_t hbm_window_mib = 0;         /* 0 = auto (fraction of free HBM) */
    uint64_t stage_mib = 1024;           /* pinned host staging / D2H+write slice */
    bool     dry_run = false;            /* label + D2H, no disk write */
    bool     confirm = false;            /* required to write real data */
    std::string mode = "disk-per-gpu";   /* alias: disk-per-gpu | gpus-per-disk */
    int      gpus_per_disk_k = 0;        /* explicit K GPUs per disk; 0 = from mode */
    uint64_t verify_n = 0;               /* sampled read-back super-chunks (0 = off) */
    bool     verify_only = false;        /* only verify existing labels, no wipe */
    bool     verbose = false;            /* raw labeler logs instead of live dashboard */
};

/* ── one contiguous wipe assignment for one GPU worker ─────────────────────── */
struct WorkItem {
    int         fd;            /* open O_DIRECT block device */
    std::string path;         /* for reporting */
    uint64_t    disk_off;     /* absolute byte offset on device (super-chunk aligned) */
    uint64_t    len;          /* bytes this worker writes (whole super-chunks) */
};

struct WorkerResult {
    int      gpu = -1;
    uint64_t bytes = 0;
    double   label_s = 0, d2h_s = 0, write_s = 0, wall_s = 0;
    bool     ok = true;
    std::string err;
};

/* ── helpers ──────────────────────────────────────────────────────────────── */
static uint64_t floor_to(uint64_t v, uint64_t m) { return m ? (v / m) * m : 0; }

static void human_bytes(double n, char *buf, size_t sz)
{
    const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int i = 0;
    while (n >= 1024.0 && i < 4) { n /= 1024.0; i++; }
    snprintf(buf, sz, "%.2f %s", n, u[i]);
}

static void human_dur(double s, char *buf, size_t sz)
{
    if (s < 60)        snprintf(buf, sz, "%.1f s", s);
    else if (s < 3600) snprintf(buf, sz, "%dm %ds", (int)(s/60), (int)s % 60);
    else if (s < 86400)snprintf(buf, sz, "%dh %dm", (int)(s/3600), (int)(s/60)%60);
    else               snprintf(buf, sz, "%.1f days", s/86400.0);
}

/* pwrite the whole buffer, looping over short writes (write(2) caps ~2 GiB).
 * The cap is page-aligned, so buf+done and off+done stay sector-aligned for an
 * O_DIRECT fd.  Returns 0 on success, -1 (errno set) on error. */
static int pwrite_all(int fd, const uint8_t *buf, size_t len, off_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, buf + done, len - done, off + (off_t)done);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { errno = EIO; return -1; }
        done += (size_t)n;
    }
    return 0;
}

/* pread the whole buffer, looping over short reads (same 2 GiB cap as write). */
static int pread_all(int fd, uint8_t *buf, size_t len, off_t off)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, buf + done, len - done, off + (off_t)done);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) { errno = EIO; return -1; }   /* short of expected length */
        done += (size_t)n;
    }
    return 0;
}

/* basename of a /dev path */
static std::string base_name(const std::string &p)
{
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

/* Map a partition/disk device name to its parent whole-disk name:
 *   nvme0n1p3 -> nvme0n1 ,  mmcblk0p2 -> mmcblk0 ,  sda3 -> sda ,  sda -> sda */
static std::string parent_disk(const std::string &dev)
{
    std::string n = base_name(dev);
    bool p_style = n.find("nvme") != std::string::npos ||
                   n.find("mmcblk") != std::string::npos ||
                   n.find("loop") != std::string::npos ||
                   n.find("nbd") != std::string::npos ||
                   n.rfind("md", 0) == 0 ||        /* md0, md127, md0p1 */
                   n.rfind("dm-", 0) == 0;         /* dm-0 (device-mapper) */
    if (p_style) {
        /* Strip a trailing partition suffix p<digits> — but ONLY when a digit
         * precedes the 'p' (nvme0n1p3, mmcblk0p2, loop0p1).  Otherwise the 'p'
         * is part of the device name itself (loop9, nbd3) and the trailing
         * digits are the device number, not a partition.  md127 / dm-0 are
         * whole devices in their own right, so they must not collapse to
         * "md" / "dm-" and falsely match every other array. */
        size_t i = n.size();
        while (i > 0 && isdigit((unsigned char)n[i - 1])) i--;
        if (i < n.size() && i >= 2 && n[i - 1] == 'p' &&
            isdigit((unsigned char)n[i - 2]))
            return n.substr(0, i - 1);
        return n;
    }
    /* sd / vd / hd style: strip trailing digits */
    size_t i = n.size();
    while (i > 0 && isdigit((unsigned char)n[i - 1])) i--;
    return i > 0 ? n.substr(0, i) : n;
}

/*
 * Refuse to touch any target that is mounted or holds a mounted partition (this
 * includes the boot/root disk).  Returns "" if safe, else a reason string.
 * Compares each target against the parent disk of every mount source in
 * /proc/mounts, so /dev/nvme0n1 is refused when /dev/nvme0n1p2 is mounted.
 */
static std::string mounted_reason(const std::string &target)
{
    std::string tgt_disk = parent_disk(target);
    std::ifstream f("/proc/mounts");
    if (!f) return "";   /* cannot read mounts — do not block on this alone */
    std::string src, mnt, rest;
    while (f >> src >> mnt) {
        std::getline(f, rest);
        if (src.rfind("/dev/", 0) != 0) continue;
        if (parent_disk(src) == tgt_disk) {
            return "device (or a partition of it) is mounted: " + src + " on " + mnt;
        }
    }
    return "";
}

/* ── open + validate one target device ────────────────────────────────────── */
static int open_disk(const std::string &path_in, bool read_only, uint64_t *size_out,
                     int *sector_out, std::string *err)
{
    /* Resolve symlinks (/dev/disk/by-id/..., /dev/disk/by-path/...) so the
     * mount check below compares real device names, not the link's basename. */
    std::string path = path_in;
    {
        char rp[PATH_MAX];
        if (realpath(path_in.c_str(), rp) != nullptr) path = rp;
    }

    struct stat st;
    if (stat(path.c_str(), &st) != 0) { *err = std::string("stat: ") + strerror(errno); return -1; }
    if (!S_ISBLK(st.st_mode)) { *err = "not a block device"; return -1; }

    std::string m = mounted_reason(path);
    if (!m.empty()) { *err = m; return -1; }

    /* For real writes open O_EXCL: the kernel refuses an exclusive open of a
     * block device that is mounted, held by device-mapper (LVM/LUKS) or md
     * RAID, or active as swap — and refuses the whole disk when any of its
     * partitions is held.  This is the same guard wipefs/mkfs rely on and it
     * covers everything the /proc/mounts scan above cannot see (an LVM PV, a
     * LUKS container, an md member, a swap partition).  Read-only verify does
     * not need it. */
    int flags = (read_only ? O_RDONLY : (O_RDWR | O_EXCL)) | O_DIRECT;
    int fd = open(path.c_str(), flags);
    if (fd < 0) {
        if (!read_only && errno == EBUSY)
            *err = "device is in use (mounted, LVM/LUKS/md member, or swap) — "
                   "refusing to open it exclusively";
        else
            *err = std::string("open: ") + strerror(errno);
        return -1;
    }

    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0 || bytes == 0) {
        *err = "BLKGETSIZE64 failed"; close(fd); return -1;
    }
    int sector = 512;
    ioctl(fd, BLKSSZGET, &sector);

    *size_out = bytes;
    *sector_out = sector;
    return fd;
}

/* ── verify: sampled read-back + CPU recompute ─────────────────────────────────
 * Reads `samples` super-chunks spread across the wiped range (always the first
 * and last), recomputes each on the CPU with the byte-identical in-place labeler
 * (pose_graph_label_region_pooled_inplace) for the same seed + chunk_index, and
 * compares.  A match proves the GPU wrote the correct PoSE-DB output-set labels —
 * exactly what a verifier's challenge checks, minus the network + RTT timing.
 * Returns 0 on success (io/setup ok); *mism_out counts mismatched super-chunks. */
static int verify_disk(int fd, uint64_t wipe_len, uint32_t cb, uint64_t samples,
                       uint64_t *checked_out, uint64_t *mism_out, std::string *err)
{
    *checked_out = 0; *mism_out = 0;
    const uint64_t super_bytes = (uint64_t)pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;
    const uint64_t n_super = wipe_len / super_bytes;
    if (n_super == 0) { *err = "nothing labeled to verify"; return -1; }
    if (samples == 0) samples = 1;
    if (samples > n_super) samples = n_super;

    uint8_t *disk_buf = nullptr, *ref = nullptr;
    uint8_t *scratch[POSE_REGION_THREADS] = {0};
    pose_graph_pool_t *pool = nullptr;
    int rc = -1;

    if (posix_memalign((void **)&disk_buf, 4096, super_bytes) != 0) {
        *err = "posix_memalign(disk_buf) failed"; disk_buf = nullptr; goto done;
    }
    ref = (uint8_t *)malloc(super_bytes);
    if (!ref) { *err = "malloc(ref) failed"; goto done; }

    {
        size_t sb = pose_graph_scratch_bytes_inplace(cb);
        bool alloc_ok = true;
        for (int i = 0; i < POSE_REGION_THREADS; i++) {
            scratch[i] = (uint8_t *)malloc(sb);
            if (!scratch[i]) alloc_ok = false;
        }
        pool = pose_graph_pool_create();
        if (!alloc_ok || !pool) { *err = "scratch/pool alloc failed"; goto done; }

        uint64_t checked = 0, mism = 0;
        for (uint64_t k = 0; k < samples; k++) {
            uint64_t J = (samples == 1) ? 0 : k * (n_super - 1) / (samples - 1);
            if (pread_all(fd, disk_buf, super_bytes, (off_t)(J * super_bytes)) != 0) {
                *err = std::string("pread: ") + strerror(errno); goto done;
            }
            /* region_block_offset = J*cb -> chunk_index J, matching the wipe. */
            if (pose_graph_label_region_pooled_inplace(
                    g_seed, sizeof(g_seed), ref, super_bytes, (uint64_t)J * cb,
                    scratch, pool, POSE_HASH_BLAKE3, cb) != 0) {
                *err = "CPU recompute failed"; goto done;
            }
            checked++;
            if (memcmp(ref, disk_buf, super_bytes) != 0) {
                mism++;
                if (mism <= 3) {
                    uint64_t bad = 0;
                    for (uint64_t r = 0; r < cb; r++)
                        if (memcmp(ref + r * 32, disk_buf + r * 32, 32) != 0) { bad = r; break; }
                    fprintf(stderr, "  verify: MISMATCH super-chunk %llu (block %llu)\n",
                            (unsigned long long)J, (unsigned long long)bad);
                }
            }
        }
        *checked_out = checked;
        *mism_out = mism;
        rc = 0;
    }

done:
    for (int i = 0; i < POSE_REGION_THREADS; i++) free(scratch[i]);
    if (pool) pose_graph_pool_destroy(pool);
    free(ref);
    free(disk_buf);
    return rc;
}

/* ── live status dashboard ─────────────────────────────────────────────────────
 * One row per GPU + an aggregate, refreshed in place (htop-style) on a TTY, or a
 * throttled one-line summary when stdout is redirected.  Workers update their
 * row's stats; a renderer thread redraws.  While the dashboard is active the
 * shared labeler is silenced via POSE_GPU_QUIET so nothing interleaves. */
enum { ST_IDLE = 0, ST_LABEL, ST_WRITE, ST_VERIFY, ST_DONE, ST_ERROR };

struct GpuStat {
    int         gpu = -1;
    uint64_t    done = 0;     /* bytes processed in the current item   */
    uint64_t    total = 0;    /* bytes in the current item             */
    double      mibs = 0.0;   /* current item average rate             */
    int         state = ST_IDLE;
    std::string disk;
};
static std::vector<GpuStat> g_stats;              /* one per selected GPU, fixed after init */
static std::mutex           g_stat_mu;
static std::atomic<uint64_t> g_written{0};        /* grand total bytes processed (all GPUs) */
static uint64_t             g_grand_total = 0;    /* grand total to process (set in main)   */
static std::atomic<bool>    g_render_stop{false};

static int gpu_row(int gpu)
{
    for (size_t i = 0; i < g_stats.size(); i++)
        if (g_stats[i].gpu == gpu) return (int)i;
    return -1;
}

static void stat_update(int gpu, uint64_t done, uint64_t total, double mibs,
                        int state, const char *disk)
{
    int i = gpu_row(gpu);
    if (i < 0) return;
    std::lock_guard<std::mutex> lk(g_stat_mu);
    GpuStat &s = g_stats[i];
    if (disk) s.disk = disk;
    s.done = done; s.total = total; s.mibs = mibs; s.state = state;
}

static std::string hms(double s)
{
    if (!(s >= 0.0) || !std::isfinite(s)) return "--:--";
    long t = (long)(s + 0.5);
    if (t > 359999) t = 359999;      /* clamp to 99:59:59 so the field can't overflow */
    char b[32];
    if (t >= 3600) snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
    else           snprintf(b, sizeof b, "%02ld:%02ld", t / 60, t % 60);
    return b;
}

static std::string bar(double frac, int w)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int f = (int)(frac * w + 0.5);
    std::string b(w, '-');
    for (int i = 0; i < f; i++) b[i] = '#';
    return b;
}

/* Current terminal width in columns, or 0 if unknown (piped / no TTY). */
static int term_width(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return (int)ws.ws_col;
    return 0;
}

static const char *st_name(int s)
{
    switch (s) {
        case ST_LABEL:  return "label";
        case ST_WRITE:  return "write";
        case ST_VERIFY: return "verify";
        case ST_DONE:   return "done";
        case ST_ERROR:  return "ERROR";
        default:        return "idle";
    }
}

/* Pick the first candidate that fits in W columns (leaving one spare so the line
 * never touches the right edge and auto-wraps); else the smallest, truncated. */
static std::string pick_fit(int W, std::initializer_list<std::string> cands)
{
    if (W <= 0) W = 80;
    const std::string *last = nullptr;
    for (const std::string &c : cands) {
        last = &c;
        if ((int)c.size() <= W - 1) return c;
    }
    std::string r = last ? *last : std::string();
    if ((int)r.size() > W - 1 && W > 1) r.resize(W - 1);
    return r;
}

/* Responsive per-GPU row: full → drop bar → drop ETA → drop rate → drop disk. */
static std::string fit_row(int W, int gpu, const std::string &disk_in,
                           double frac, double mibs, double eta, int state)
{
    const char *st = st_name(state);
    std::string dn = disk_in.empty() ? "-" : disk_in;
    if (dn.size() > 12) dn.resize(12);
    char full[256], b10[256], nob[256], rate[128], pct[64], mini[32];
    snprintf(full, sizeof full, "GPU %-2d %-12s [%s] %5.1f%% %8.1f MiB/s  ETA %-7s %s",
             gpu, dn.c_str(), bar(frac, 20).c_str(), frac * 100.0, mibs, hms(eta).c_str(), st);
    snprintf(b10, sizeof b10, "GPU %-2d %-12s [%s] %5.1f%% %7.1f MiB/s %s",
             gpu, dn.c_str(), bar(frac, 10).c_str(), frac * 100.0, mibs, st);
    snprintf(nob, sizeof nob, "GPU %-2d %-10s %5.1f%% %7.1f MiB/s %s",
             gpu, dn.c_str(), frac * 100.0, mibs, st);
    snprintf(rate, sizeof rate, "GPU %-2d %5.1f%% %7.1f MiB/s %s", gpu, frac * 100.0, mibs, st);
    snprintf(pct, sizeof pct, "GPU %-2d %5.1f%% %s", gpu, frac * 100.0, st);
    snprintf(mini, sizeof mini, "G%d %4.0f%%", gpu, frac * 100.0);
    return pick_fit(W, {full, b10, nob, rate, pct, mini});
}

/* Responsive aggregate row, same progression. */
static std::string fit_agg(int W, const char *wt, const char *gt,
                           double frac, double agg, double eta, double el)
{
    char full[256], b10[256], nob[128], pr[96], mini[32];
    snprintf(full, sizeof full, "ALL    %s / %-9s [%s] %5.1f%% %8.1f MiB/s  ETA %-7s elapsed %s",
             wt, gt, bar(frac, 20).c_str(), frac * 100.0, agg, hms(eta).c_str(), hms(el).c_str());
    snprintf(b10, sizeof b10, "ALL    %s / %-9s [%s] %5.1f%% %7.1f MiB/s  elapsed %s",
             wt, gt, bar(frac, 10).c_str(), frac * 100.0, agg, hms(el).c_str());
    snprintf(nob, sizeof nob, "ALL    %s / %-9s %5.1f%% %7.1f MiB/s", wt, gt, frac * 100.0, agg);
    snprintf(pr, sizeof pr, "ALL  %5.1f%% %7.1f MiB/s", frac * 100.0, agg);
    snprintf(mini, sizeof mini, "ALL %4.0f%%", frac * 100.0);
    return pick_fit(W, {full, b10, nob, pr, mini});
}

static void render_frame(double t0, bool tty, bool final_frame)
{
    static bool   first = true;
    static int    prev_lines = 0;
    static double last_plain = -1e9;

    double now = now_sec();
    std::vector<GpuStat> snap;
    { std::lock_guard<std::mutex> lk(g_stat_mu); snap = g_stats; }

    uint64_t written = g_written.load();
    double el   = now - t0;
    double agg  = el > 0 ? (double)written / MIB / el : 0.0;
    double frac = g_grand_total ? (double)written / (double)g_grand_total : 0.0;
    double eta  = agg > 0 ? ((double)g_grand_total - (double)written) / MIB / agg : INFINITY;

    if (!tty) {
        if (!final_frame && now - last_plain < 5.0) return;   /* throttle when piped */
        last_plain = now;
        char wb[32]; human_bytes((double)written, wb, sizeof wb);
        printf("[%s] %s  %.1f%%  agg %.1f MiB/s  eta %s\n",
               hms(el).c_str(), wb, frac * 100.0, agg, hms(eta).c_str());
        for (auto &s : snap) {
            if (s.state == ST_IDLE && s.done == 0) continue;
            double fr = s.total ? (double)s.done / (double)s.total : 0.0;
            printf("    gpu %d  %-14s %5.1f%%  %7.1f MiB/s  %s\n",
                   s.gpu, s.disk.empty() ? "-" : s.disk.c_str(), fr * 100.0, s.mibs, st_name(s.state));
        }
        fflush(stdout);
        return;
    }

    /* Build rows sized to the terminal: fields drop as it narrows, and nothing
     * wraps (a wrapped line breaks the cursor-up count → garbled display). */
    int width = term_width();
    std::vector<std::string> lines;
    for (auto &s : snap) {
        double fr   = s.total ? (double)s.done / (double)s.total : 0.0;
        double reta = s.mibs > 0 ? ((double)s.total - (double)s.done) / MIB / s.mibs : INFINITY;
        lines.push_back(fit_row(width, s.gpu, s.disk, fr, s.mibs, reta, s.state));
    }
    char gt[32], wt[32];
    human_bytes((double)g_grand_total, gt, sizeof gt);
    human_bytes((double)written, wt, sizeof wt);
    lines.push_back(fit_agg(width, wt, gt, frac, agg, eta, el));

    std::lock_guard<std::mutex> lk(g_print_mu);
    if (!first) printf("\033[%dA", prev_lines);   /* move cursor back to top of block */
    for (auto &ln : lines) printf("\r\033[2K%s\n", ln.c_str());
    prev_lines = (int)lines.size();
    first = false;
    fflush(stdout);
}

static void renderer_thread(double t0, bool tty)
{
    while (!g_render_stop.load()) {
        render_frame(t0, tty, false);
        for (int i = 0; i < 5 && !g_render_stop.load(); i++) usleep(100000);  /* ~500ms */
    }
    render_frame(t0, tty, true);   /* final frame */
}

/* ── the GPU worker: wipe one or more contiguous ranges assigned to it ─────── */
static void worker(int gpu, std::vector<WorkItem> items, Config cfg, WorkerResult *res)
{
    res->gpu = gpu;
    double wall0 = now_sec();

    if (cudaSetDevice(gpu) != cudaSuccess) {
        res->ok = false; res->err = "cudaSetDevice failed"; return;
    }

    const uint32_t cb = cfg.chunk_blocks;
    const uint64_t super_bytes = (uint64_t)pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;

    /* A worker with nothing to do (a disk with fewer super-chunks than team
     * GPUs) must not build a topology or grab an HBM window for nothing. */
    uint64_t total_len = 0;
    for (const WorkItem &it : items) total_len += it.len;
    if (total_len == 0) {
        stat_update(gpu, 0, 0, 0.0, ST_DONE, nullptr);
        res->wall_s = now_sec() - wall0;
        return;
    }

    /* Build the seed-independent scaffold topology ONCE and reuse it for every
     * HBM window and every disk range this worker labels — the topology (host
     * build + H2D upload) is the dominant fixed cost, so rebuilding it per window
     * (as a bare pose_graph_label_hbm call would) is what dragged the aggregate
     * below the per-window label rate. */
    pose_hbm_topo_t *topo = pose_graph_hbm_topo_build(cb, POSE_HASH_BLAKE3);
    if (!topo) {
        res->ok = false; res->err = "pose_graph_hbm_topo_build failed"; return;
    }

    /* HBM label window: bounded by free VRAM (after the topology alloc), leaving
     * the majority free for the labeler's transient work buffer, which drives
     * throughput.  Never allocate more than the largest range this worker will
     * actually process — no point grabbing 30% of a 140 GB card to wipe 8 GiB
     * (also friendlier on a shared machine). */
    size_t free_b = 0, total_b = 0;
    if (cudaMemGetInfo(&free_b, &total_b) != cudaSuccess) {
        pose_graph_hbm_topo_free(topo);
        res->ok = false; res->err = "cudaMemGetInfo failed"; return;
    }
    uint64_t max_item = 0;
    for (const WorkItem &it : items) max_item = std::max(max_item, it.len);

    /* Default window is 16 GiB (capped at 30% of free VRAM).  The labeler's
     * concurrency is bounded by the super-chunks in one window: the warp strategy
     * keeps one super-chunk per resident warp (3168 on a 132-SM part = 12.4 GiB
     * at cb = 2^17), so a 4 GiB window starved it to ~1 block/SM.  16 GiB keeps
     * every resident warp busy while still leaving room for the ~25 GiB warp
     * scratch pool on an 80 GB H100.  --hbm-window-mib overrides it (taken as
     * given — the cudaMalloc below fails loudly if it does not fit); both are
     * bounded by the largest range this worker will process. */
    uint64_t window = cfg.hbm_window_mib
                        ? cfg.hbm_window_mib * MIB
                        : std::min<uint64_t>((uint64_t)((double)free_b * 0.30),
                                             16384ull * MIB);
    if (max_item && window > max_item) window = max_item;   /* don't over-allocate */
    window = floor_to(window, super_bytes);
    if (window < super_bytes) window = super_bytes;      /* at least one super-chunk */

    uint64_t stage = floor_to(cfg.stage_mib * MIB, super_bytes);
    if (stage < super_bytes) stage = super_bytes;
    if (stage > window) stage = window;

    void *d_buf = nullptr;
    if (cudaMalloc(&d_buf, window) != cudaSuccess) {
        pose_graph_hbm_topo_free(topo);
        res->ok = false; res->err = "cudaMalloc(label window) failed"; return;
    }
    uint8_t *h_stage = nullptr;
    if (cudaMallocHost((void **)&h_stage, stage) != cudaSuccess) {
        cudaFree(d_buf);
        pose_graph_hbm_topo_free(topo);
        res->ok = false; res->err = "cudaMallocHost(stage) failed"; return;
    }

    for (const WorkItem &it : items) {
        if (!res->ok) break;
        std::string dname = base_name(it.path);
        double item_t0 = now_sec();
        uint64_t pos = 0;
        stat_update(gpu, 0, it.len, 0.0, ST_LABEL, dname.c_str());
        while (pos < it.len) {
            uint64_t win = std::min(window, it.len - pos);
            win = floor_to(win, super_bytes);
            if (win == 0) break;   /* trailing bytes < one super-chunk: left as-is */

            uint64_t rbo = (it.disk_off + pos) / POSE_HASH_BYTES;   /* absolute block */

            stat_update(gpu, pos, it.len,
                        pos / MIB / std::max(1e-9, now_sec() - item_t0), ST_LABEL, nullptr);
            double t0 = now_sec();
            /* Reuses the resident topology — no per-window rebuild.  This call
             * synchronises internally, so the elapsed time is real device work. */
            if (pose_graph_hbm_topo_label(topo, g_seed, sizeof(g_seed),
                                          d_buf, win, rbo) != 0) {
                res->ok = false; res->err = "pose_graph_hbm_topo_label failed"; break;
            }
            res->label_s += now_sec() - t0;

            for (uint64_t s = 0; s < win && res->ok; s += stage) {
                uint64_t slice = std::min(stage, win - s);
                double td = now_sec();
                if (cudaMemcpy(h_stage, (const uint8_t *)d_buf + s, slice,
                               cudaMemcpyDeviceToHost) != cudaSuccess) {
                    res->ok = false; res->err = "cudaMemcpy D2H failed"; break;
                }
                res->d2h_s += now_sec() - td;

                if (!cfg.dry_run) {
                    double tw = now_sec();
                    if (pwrite_all(it.fd, h_stage, slice,
                                   (off_t)(it.disk_off + pos + s)) != 0) {
                        res->ok = false;
                        res->err = std::string("pwrite: ") + strerror(errno);
                        break;
                    }
                    res->write_s += now_sec() - tw;
                }
                res->bytes += slice;
                g_written.fetch_add(slice);
            }
            pos += win;

            double tn = now_sec();
            double rate = pos / MIB / std::max(1e-9, tn - item_t0);
            stat_update(gpu, pos, it.len, rate, cfg.dry_run ? ST_LABEL : ST_WRITE, nullptr);
            if (cfg.verbose && (tn - item_t0 >= 5.0 || pos >= it.len)) {
                std::lock_guard<std::mutex> lk(g_print_mu);
                printf("  [gpu %d] %s  %.1f%% of item  (%.1f MiB/s)\n",
                       gpu, dname.c_str(), 100.0 * (double)pos / (double)it.len, rate);
                fflush(stdout);
            }
        }
        if (res->ok && !cfg.dry_run) fsync(it.fd);
        stat_update(gpu, it.len, it.len,
                    it.len / MIB / std::max(1e-9, now_sec() - item_t0),
                    res->ok ? ST_DONE : ST_ERROR, nullptr);
    }

    cudaFreeHost(h_stage);
    cudaFree(d_buf);
    pose_graph_hbm_topo_free(topo);
    res->wall_s = now_sec() - wall0;
}

/* ── argument parsing ─────────────────────────────────────────────────────── */
static void usage(const char *argv0)
{
    fprintf(stderr,
"Usage: %s --disk /dev/DEV [--disk /dev/DEV ...] [options]\n"
"\n"
"Fills the chosen block device(s) with GPU-generated PoSE-DB labels (a real\n"
"wipe) and reports throughput.  Only devices named with --disk are touched.\n"
"\n"
"Options:\n"
"  --disk PATH          target block device (repeatable; required)\n"
"  --gpus LIST          CUDA ordinals to use, e.g. 0,1,2,3 (default: all visible)\n"
"  --gpus-per-disk K    K GPUs team up per disk; floor(pool/K) disks in parallel.\n"
"                       Overrides --mode. e.g. --gpus 0,1,2,3 --gpus-per-disk 2\n"
"                       wipes 2 disks at once, 2 GPUs each.\n"
"  --mode MODE          alias for the extremes: disk-per-gpu (K=1, default) |\n"
"                       gpus-per-disk (K=all selected GPUs)\n"
"  --chunk-blocks N     graph size, power of two 1..2^20 (default %u)\n"
"  --per-disk-mib N     cap bytes written per disk for a quick test\n"
"                       (default 0 = the ENTIRE device)\n"
"  --hbm-window-mib N   HBM label buffer per GPU (default 0 = 16 GiB, capped at\n"
"                       30%% of free VRAM and at the range the GPU will process)\n"
"  --stage-mib N        pinned host D2H/write slice (default 1024)\n"
"  --dry-run            label + device->host copy only; NO disk writes (safe)\n"
"  --confirm            required to perform real (destructive) disk writes\n"
"  --verify N           after the wipe, read back N sampled super-chunks and\n"
"                       CPU-recompute them to confirm correct labels (0 = off)\n"
"  --verify-only        do NOT wipe; only verify existing labels (opens read-only,\n"
"                       needs the same --chunk-blocks/--seed-hex the wipe used)\n"
"  --verbose            disable the live dashboard; print raw per-window labeler\n"
"                       logs instead (useful for debugging / non-interactive logs)\n"
"  --seed-hex HEX       64 hex chars = 32-byte session seed (default fixed)\n"
"  -h, --help           this help\n",
        argv0, POSE_CHUNK_BLOCKS);
}

static int parse_hex_seed(const char *hex, uint8_t out[POSE_HASH_BYTES])
{
    if (strlen(hex) != POSE_HASH_BYTES * 2) return -1;
    for (int i = 0; i < POSE_HASH_BYTES; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    Config cfg;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](const char *n) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "%s requires a value\n", n); exit(2); }
            return argv[++i];
        };
        if      (a == "--disk")            cfg.disks.push_back(need("--disk"));
        else if (a == "--mode") {
            cfg.mode = need("--mode");
            if (cfg.mode != "gpus-per-disk" && cfg.mode != "disk-per-gpu") {
                fprintf(stderr, "unknown --mode %s (use disk-per-gpu | gpus-per-disk)\n",
                        cfg.mode.c_str());
                return 2;
            }
        }
        else if (a == "--gpus-per-disk")   cfg.gpus_per_disk_k = atoi(need("--gpus-per-disk"));
        else if (a == "--verify")          cfg.verify_n = strtoull(need("--verify"), nullptr, 10);
        else if (a == "--verify-only")     cfg.verify_only = true;
        else if (a == "--verbose")         cfg.verbose = true;
        else if (a == "--chunk-blocks")    cfg.chunk_blocks = (uint32_t)strtoul(need("--chunk-blocks"), nullptr, 10);
        else if (a == "--per-disk-mib")    cfg.per_disk_bytes = strtoull(need("--per-disk-mib"), nullptr, 10) * MIB;
        else if (a == "--hbm-window-mib")  cfg.hbm_window_mib = strtoull(need("--hbm-window-mib"), nullptr, 10);
        else if (a == "--stage-mib")       cfg.stage_mib = strtoull(need("--stage-mib"), nullptr, 10);
        else if (a == "--dry-run")         cfg.dry_run = true;
        else if (a == "--confirm")         cfg.confirm = true;
        else if (a == "--gpus") {
            std::string l = need("--gpus");
            size_t p = 0;
            while (p < l.size()) {
                size_t c = l.find(',', p);
                std::string tok = l.substr(p, c == std::string::npos ? std::string::npos : c - p);
                char *end = nullptr;
                long v = strtol(tok.c_str(), &end, 10);
                if (tok.empty() || !end || *end != '\0' || v < 0 || v > INT_MAX) {
                    fprintf(stderr, "--gpus: bad ordinal '%s' (expected e.g. 0,1,2,3)\n", tok.c_str());
                    return 2;
                }
                if (std::find(cfg.gpus.begin(), cfg.gpus.end(), (int)v) != cfg.gpus.end()) {
                    fprintf(stderr, "--gpus: ordinal %ld listed twice\n", v);
                    return 2;
                }
                cfg.gpus.push_back((int)v);
                if (c == std::string::npos) break;
                p = c + 1;
            }
        }
        else if (a == "--seed-hex") {
            if (parse_hex_seed(need("--seed-hex"), g_seed) != 0) {
                fprintf(stderr, "--seed-hex must be 64 hex characters\n"); return 2;
            }
        }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    if (cfg.disks.empty()) {
        fprintf(stderr, "error: at least one --disk is required (no auto-selection)\n");
        usage(argv[0]);
        return 2;
    }
    if (cfg.chunk_blocks == 0) cfg.chunk_blocks = POSE_CHUNK_BLOCKS;
    if ((cfg.chunk_blocks & (cfg.chunk_blocks - 1)) != 0 ||
        cfg.chunk_blocks > POSE_GRAPH_MAX_M) {
        fprintf(stderr, "error: --chunk-blocks must be a power of two in [1, %u]\n",
                POSE_GRAPH_MAX_M);
        return 2;
    }
    if (cfg.verify_only && cfg.verify_n == 0) cfg.verify_n = 128;
    if (cfg.verify_n > 0 && cfg.dry_run && !cfg.verify_only)
        fprintf(stderr, "warning: --verify is ignored with --dry-run (nothing is written "
                        "to read back); use --verify-only to check an existing wipe\n");

    /* GPU inventory */
    int ndev = 0;
    cudaError_t derr = cudaGetDeviceCount(&ndev);
    if (derr != cudaSuccess) {
        fprintf(stderr, "error: CUDA cannot access any GPU: %s (%d)\n",
                cudaGetErrorString(derr), (int)derr);
        fprintf(stderr,
            "  Driver/runtime problem, not the tool.  Check, in order:\n"
            "    nvidia-smi                 # driver loaded + lists GPUs?\n"
            "    lsmod | grep nvidia        # nvidia + nvidia_uvm modules loaded?\n"
            "    ls -l /dev/nvidia*         # device nodes present?\n"
            "    cat /proc/driver/nvidia/version\n"
            "  If nvidia-smi's 'CUDA Version' is below the toolkit this binary was\n"
            "  built with, the installed driver is too old for the CUDA runtime —\n"
            "  reboot after a driver install, run `sudo modprobe nvidia_uvm`, or\n"
            "  install a newer driver.\n");
        return 1;
    }
    if (ndev < 1) {
        fprintf(stderr, "error: driver is up but reports 0 CUDA devices\n");
        return 1;
    }
    if (cfg.gpus.empty())
        for (int i = 0; i < ndev; i++) cfg.gpus.push_back(i);
    for (int g : cfg.gpus) {
        if (g < 0 || g >= ndev) {
            fprintf(stderr, "error: GPU ordinal %d out of range (0..%d)\n", g, ndev - 1);
            return 2;
        }
    }

    const uint32_t cb = cfg.chunk_blocks;
    const uint64_t super_bytes = (uint64_t)pose_graph_super_chunk_blocks(cb) * POSE_HASH_BYTES;

    /* Open + validate every target BEFORE any write. */
    struct OpenDisk { std::string path; int fd; uint64_t size; int sector; uint64_t wipe_len; };
    std::vector<OpenDisk> disks;
    for (const std::string &p : cfg.disks) {
        uint64_t size; int sector; std::string err;
        int fd = open_disk(p, cfg.verify_only, &size, &sector, &err);
        if (fd < 0) {
            fprintf(stderr, "error: %s: %s\n", p.c_str(), err.c_str());
            for (auto &d : disks) close(d.fd);
            return 1;
        }
        if (super_bytes % (uint64_t)sector != 0) {
            fprintf(stderr, "error: %s: super-chunk %llu B is not a multiple of the "
                    "%d B sector; use a chunk_blocks that is a multiple of %d "
                    "(e.g. 131072)\n", p.c_str(),
                    (unsigned long long)super_bytes, sector, sector / POSE_HASH_BYTES);
            close(fd);
            for (auto &d : disks) close(d.fd);
            return 1;
        }
        uint64_t want = cfg.per_disk_bytes ? std::min(cfg.per_disk_bytes, size) : size;
        uint64_t wipe_len = floor_to(want, super_bytes);
        if (wipe_len == 0) {
            fprintf(stderr, "error: %s: device smaller than one super-chunk (%llu B)\n",
                    p.c_str(), (unsigned long long)super_bytes);
            close(fd);
            for (auto &d : disks) close(d.fd);
            return 1;
        }
        disks.push_back({p, fd, size, sector, wipe_len});
    }

    /* ── verify-only: no wipe, just read back + CPU-recompute existing labels ── */
    if (cfg.verify_only) {
        printf("=== disk-wipe-bench: verify-only ===\n");
        printf("chunk_blocks  : %u  (super-chunk %llu B)  samples/disk: %llu\n",
               cb, (unsigned long long)super_bytes, (unsigned long long)cfg.verify_n);
        int vrc = 0;
        for (auto &d : disks) {
            uint64_t checked = 0, mism = 0; std::string verr;
            if (verify_disk(d.fd, d.wipe_len, cb, cfg.verify_n, &checked, &mism, &verr) != 0) {
                printf("  %s: verify ERROR: %s\n", d.path.c_str(), verr.c_str());
                vrc = 1;
            } else {
                printf("  %s: %llu/%llu sampled super-chunks match%s\n", d.path.c_str(),
                       (unsigned long long)(checked - mism), (unsigned long long)checked,
                       mism ? "   <-- MISMATCH" : "");
                if (mism) vrc = 1;
            }
        }
        for (auto &d : disks) close(d.fd);
        printf("%s\n", vrc ? "VERIFY FAILED" : "VERIFY OK");
        return vrc;
    }

    /* ── resolve scheduling: K GPUs per disk, T = floor(pool/K) disks in flight ── */
    int G = (int)cfg.gpus.size();
    int K = cfg.gpus_per_disk_k > 0 ? cfg.gpus_per_disk_k
          : (cfg.mode == "gpus-per-disk" ? G : 1);
    if (K < 1) K = 1;
    if (K > G) {
        fprintf(stderr, "warning: --gpus-per-disk %d exceeds %d selected GPU(s); using %d\n",
                cfg.gpus_per_disk_k, G, G);
        K = G;
    }
    int T = G / K;                 /* concurrent teams = disks wiped in parallel */
    int unused_gpus = G - T * K;

    /* ── plan banner ── */
    printf("=== disk-wipe-bench ===\n");
    printf("schedule      : %d GPU(s)/disk, %d disk(s) in parallel%s\n", K, T,
           K == 1 ? "  [disk-per-gpu]" : (T == 1 ? "  [gpus-per-disk]" : ""));
    if (unused_gpus > 0)
        printf("              : %d GPU(s) idle (pool of %d not divisible by K=%d)\n",
               unused_gpus, G, K);
    if ((int)disks.size() < T)
        printf("              : only %zu disk(s) given — %d team(s) will be idle\n",
               disks.size(), T - (int)disks.size());
    printf("chunk_blocks  : %u  (super-chunk %llu B)\n", cb, (unsigned long long)super_bytes);
    printf("GPUs          : ");
    for (size_t i = 0; i < cfg.gpus.size(); i++) printf("%d%s", cfg.gpus[i], i + 1 < cfg.gpus.size() ? "," : "\n");
    printf("write mode    : %s\n", cfg.dry_run ? "DRY-RUN (no disk writes)"
                                  : (cfg.confirm ? "REAL WRITES (destructive)" : "(blocked — need --confirm or --dry-run)"));
    uint64_t total_wipe = 0;
    for (auto &d : disks) {
        char hs[32], hw[32];
        human_bytes((double)d.size, hs, sizeof(hs));
        human_bytes((double)d.wipe_len, hw, sizeof(hw));
        printf("  target      : %s  size %s  wipe %s%s\n", d.path.c_str(), hs, hw,
               cfg.per_disk_bytes ? "  [capped]" : "");
        total_wipe += d.wipe_len;
    }
    { char ht[32]; human_bytes((double)total_wipe, ht, sizeof(ht));
      printf("total to write: %s\n", ht); }
    fflush(stdout);

    if (!cfg.dry_run && !cfg.confirm) {
        fprintf(stderr,
            "\nREFUSING TO WRITE: this will DESTROY all data on the target device(s).\n"
            "Re-run with --dry-run to test the pipeline safely, or --confirm to wipe.\n");
        for (auto &d : disks) close(d.fd);
        return 1;
    }

    /* ── schedule + run ──
     * T teams run concurrently (one per disk in flight).  Each team owns K
     * disjoint GPUs and works through its assigned disks in order; every disk is
     * split into K super-chunk-aligned contiguous sub-ranges, one per team GPU.
     * Each team GPU gets ONE long-lived worker carrying its sub-range on every
     * team disk, so the scaffold topology and HBM window are built once per GPU
     * for the whole run rather than once per disk. */
    double t0 = now_sec();

    /* Partition the GPU pool into T disjoint teams of K GPUs. */
    std::vector<std::vector<int>> team_gpus(T);
    for (int t = 0; t < T; t++)
        for (int j = 0; j < K; j++) team_gpus[t].push_back(cfg.gpus[t * K + j]);

    /* Assign disks to teams largest-first (round-robin over teams) for balance. */
    std::vector<size_t> order(disks.size());
    for (size_t i = 0; i < disks.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return disks[a].wipe_len > disks[b].wipe_len; });
    std::vector<std::vector<size_t>> team_disks(T);
    for (size_t k = 0; k < order.size() && T > 0; k++)
        team_disks[k % (size_t)T].push_back(order[k]);

    std::vector<std::vector<WorkerResult>> team_results(T);
    for (int t = 0; t < T; t++) team_results[t].resize(K);

    /* ── live dashboard setup ── */
    g_stats.assign(cfg.gpus.size(), GpuStat{});
    for (size_t i = 0; i < cfg.gpus.size(); i++) g_stats[i].gpu = cfg.gpus[i];
    g_grand_total = total_wipe;
    g_written.store(0);
    bool dashboard = !cfg.verbose;
    bool tty = isatty(fileno(stdout)) != 0;
    if (dashboard) {
        /* Silence the shared labeler's own progress lines so the dashboard owns
         * the screen.  (Restored to noisy on the next process; env is local.) */
        setenv("POSE_GPU_QUIET", "1", 1);
        if (tty) printf("\n");   /* blank line: the refreshing block lives below the banner */
    }
    g_render_stop.store(false);
    std::thread render_th;
    if (dashboard) render_th = std::thread(renderer_thread, t0, tty);

    std::vector<std::thread> wth;
    for (int t = 0; t < T; t++) {
        std::vector<std::vector<WorkItem>> per_gpu(K);
        for (size_t di : team_disks[t]) {
            const OpenDisk &d = disks[di];
            uint64_t nsuper = d.wipe_len / super_bytes;
            uint64_t base   = nsuper / (uint64_t)K;
            uint64_t extra  = nsuper % (uint64_t)K;
            uint64_t off = 0;
            for (int j = 0; j < K; j++) {
                uint64_t ns  = base + ((uint64_t)j < extra ? 1 : 0);
                uint64_t len = ns * super_bytes;
                if (len) per_gpu[j].push_back(WorkItem{ d.fd, d.path, off, len });
                off += len;
            }
        }
        for (int j = 0; j < K; j++)
            wth.emplace_back(worker, team_gpus[t][j], per_gpu[j], cfg, &team_results[t][j]);
    }
    for (auto &w : wth) w.join();

    if (dashboard) {
        g_render_stop.store(true);
        render_th.join();
    }

    std::vector<WorkerResult> results;
    for (auto &tr : team_results)
        for (auto &r : tr) results.push_back(r);

    double wall = now_sec() - t0;

    /* ── report ── */
    printf("\n=== results ===\n");
    uint64_t total = 0; bool ok = true;
    double sum_label = 0, sum_d2h = 0, sum_write = 0;
    for (auto &r : results) {
        total += r.bytes;
        sum_label += r.label_s; sum_d2h += r.d2h_s; sum_write += r.write_s;
        if (!r.ok) ok = false;
        char hb[32]; human_bytes((double)r.bytes, hb, sizeof(hb));
        double mibps = r.wall_s > 0 ? r.bytes / MIB / r.wall_s : 0;
        printf("  [gpu %d] %s in %.1fs = %.1f MiB/s  "
               "(label %.1fs  d2h %.1fs  write %.1fs)%s%s\n",
               r.gpu, hb, r.wall_s, mibps, r.label_s, r.d2h_s, r.write_s,
               r.ok ? "" : "  ERROR: ", r.ok ? "" : r.err.c_str());
    }
    char ht[32], hd[32]; human_bytes((double)total, ht, sizeof(ht)); human_dur(wall, hd, sizeof(hd));
    printf("-------------------------------------------------------------\n");
    printf("total written : %s\n", ht);
    printf("wall clock    : %s (%.1f s)\n", hd, wall);
    printf("aggregate     : %.1f MiB/s%s\n", wall > 0 ? total / MIB / wall : 0.0,
           cfg.dry_run ? "  [dry-run: label+D2H only, no writes]" : "");
    if (!results.empty()) {
        double busy = 0; for (auto &r : results) busy += r.wall_s;
        printf("time split    : label %.0f%%  d2h %.0f%%  write %.0f%%  (of worker busy time)\n",
               100.0 * sum_label / std::max(1e-9, busy),
               100.0 * sum_d2h  / std::max(1e-9, busy),
               100.0 * sum_write / std::max(1e-9, busy));
    }
    printf("Feed the aggregate per-GPU MiB/s to tools/disk-calc for fleet estimates.\n");

    /* ── optional post-wipe correctness check ── */
    if (cfg.verify_n > 0 && !cfg.dry_run) {
        printf("\n=== verify (sampled read-back + CPU recompute) ===\n");
        for (auto &d : disks) {
            uint64_t checked = 0, mism = 0; std::string verr;
            if (verify_disk(d.fd, d.wipe_len, cb, cfg.verify_n, &checked, &mism, &verr) != 0) {
                printf("  %s: verify ERROR: %s\n", d.path.c_str(), verr.c_str());
                ok = false;
            } else {
                printf("  %s: %llu/%llu sampled super-chunks match%s\n", d.path.c_str(),
                       (unsigned long long)(checked - mism), (unsigned long long)checked,
                       mism ? "   <-- MISMATCH" : "");
                if (mism) ok = false;
            }
        }
    }

    for (auto &d : disks) close(d.fd);
    return ok ? 0 : 1;
}
