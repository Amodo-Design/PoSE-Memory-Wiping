//! Session plans: what to label, with which parameters, in which order.
//!
//! The default plan covers the full extent of every target in the inventory,
//! each rounded down to whole super-chunks (chunk_blocks labels of 32 bytes),
//! in the order the tray needs: the deepest downstream node first, then the
//! source's own targets (it must not keep remote labels while it labels its
//! own memory); within a node disks, then RAM, then GPUs last: every
//! session whose labels the source generates (disks, RAM, and the sinks
//! before them) runs while the GPU's whole HBM is free for the generator's
//! windows and scratch, and the HBM session then overwrites that residue.
//! Each kind is ordered by base then name.  Session numbers count from 0 in
//! that order.
//!
//! The algorithm is not a choice: labels generated on a GPU are BLAKE3 (the
//! only hash the GPU implements) and labels generated on a CPU are AES-PRF
//! (the fastest there).  A source with a GPU generates every session's
//! labels on it, so its plan is all BLAKE3; a source without one gets
//! AES-PRF throughout.
//!
//! A GPU session is sized from the inventory's labelable HBM minus what the
//! GPU labeler must hold beside the buffer (pose_gpu_scratch_bytes(cb, 1):
//! the resident topology, a margin and one scratch pool — the session labels
//! its whole buffer using its own unlabeled tail as scratch), and always
//! labels with BLAKE3, the one algorithm the GPU implements.
//!
//! A plan is also a TOML file (node-qualified targets), which the verifier
//! writes beside every run's record so the run can be repeated with
//! `pose-verifier run --plan`.  It never contains seeds: a fresh one is drawn
//! for every session of every run.

use crate::ffi;
use crate::proto::{c, Entry, Kind, LABEL_BYTES};
use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use std::fmt::Write as _;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Algo {
    Blake3,
    Sha256,
    AesPrf,
}

impl Algo {
    pub fn wire(self) -> u8 {
        (match self {
            Algo::Blake3 => c::SINK_ALGO_BLAKE3,
            Algo::Sha256 => c::SINK_ALGO_SHA256,
            Algo::AesPrf => c::SINK_ALGO_AES_PRF,
        }) as u8
    }
    pub fn name(self) -> &'static str {
        match self {
            Algo::Blake3 => "blake3",
            Algo::Sha256 => "sha256",
            Algo::AesPrf => "aes_prf",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, clap::ValueEnum)]
#[serde(rename_all = "snake_case")]
pub enum Finish {
    Halt,
    Exit,
    Reboot,
}

impl Finish {
    pub fn wire(self) -> u8 {
        (match self {
            Finish::Halt => c::SINK_FINISH_HALT,
            Finish::Exit => c::SINK_FINISH_EXIT,
            Finish::Reboot => c::SINK_FINISH_REBOOT,
        }) as u8
    }
    pub fn name(self) -> &'static str {
        match self {
            Finish::Halt => "halt",
            Finish::Exit => "exit",
            Finish::Reboot => "reboot",
        }
    }
}

/// One session of a plan.  Its number is its position.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Session {
    #[serde(default)]
    pub node: String,
    pub target: String,
    pub base: u64,
    pub size: u64,
    pub chunk_blocks: u32,
    pub algo: Algo,
    pub challenges: u32,
    pub delta_ms: u64,
}

impl Session {
    pub fn blocks(&self) -> u64 {
        self.size / LABEL_BYTES as u64
    }
    /// "dpu0:ram@0x2c0000000", "/dev/nvme0n1" (own node).
    pub fn label(&self) -> String {
        target_label(&self.node, &self.target, self.base, self.target == "ram")
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Plan {
    pub finish: Finish,
    #[serde(rename = "session", default)]
    pub sessions: Vec<Session>,
}

// ── parameters ────────────────────────────────────────────────────────────

/// Sequential single-hash latency (ns) assumed for a relaying adversary.  A
/// placeholder carried over from the old verifier (and pose_calc.py): it must
/// be re-pinned by measuring on the fastest platform an adversary could field.
/// Lower latency favours the adversary.
pub const PER_HASH_LATENCY_NS: f64 = 150.0;
/// Margin the defaults keep over q (gamma >= MARGIN * q).
pub const DEFAULT_MARGIN: f64 = 1.5;
/// The default chunk_blocks (user's choice, 2026-11-10): gamma = 2^16, a
/// 4 MiB super-chunk, a 765 MiB GPU reserve.  The default delta_ms follows
/// from it (delta_ms_for) rather than the other way round.
pub const DEFAULT_CHUNK_BLOCKS: u32 = 1 << 17;
/// Fraction of labels a cheating prover is assumed to free, for the
/// (informational) soundness figure.
pub const ASSUMED_FREE_FRACTION: f64 = 0.01;
/// What the GPU labeler must hold beside an HBM session's buffer: the
/// resident topology, a margin and one scratch pool (the session labels its
/// whole buffer using the buffer's own unlabeled tail as scratch, so no
/// more is reserved; the daemon checks the same figure, gpu_sessions.c).
pub fn gpu_reserve(chunk_blocks: u32) -> u64 {
    unsafe { ffi::pose_gpu_scratch_bytes(chunk_blocks, 1) }
}

/// The bytes of a target a session may cover: its whole extent, less the
/// GPU labeler's reserve for a GPU.
pub fn usable_size(e: &Entry, chunk_blocks: u32) -> u64 {
    if e.kind == Kind::Gpu { e.size.saturating_sub(gpu_reserve(chunk_blocks)) } else { e.size }
}

/// The hard-coded run parameters (no configuration file yet).
#[derive(Clone, Copy, Debug)]
pub struct Params {
    pub chunk_blocks: u32,
    pub delta_ms: u64,
    pub challenges: u32,
    pub finish: Finish,
}

impl Default for Params {
    fn default() -> Self {
        Params {
            chunk_blocks: DEFAULT_CHUNK_BLOCKS,
            delta_ms: delta_ms_for(DEFAULT_CHUNK_BLOCKS, DEFAULT_MARGIN),
            challenges: 128,
            finish: Finish::Halt,
        }
    }
}

/// The largest whole-millisecond delta_ms at which chunk_blocks keeps gamma
/// >= `margin` times q: 6 ms for 2^17 at the assumed 150 ns per hash.
pub fn delta_ms_for(chunk_blocks: u32, margin: f64) -> u64 {
    (ffi::gamma(chunk_blocks) as f64 * PER_HASH_LATENCY_NS / (margin * 1e6)).floor() as u64
}

/// q: hashes a relay can chain sequentially within one delta_ms round trip.
pub fn q(delta_ms: u64) -> f64 {
    delta_ms as f64 * 1e6 / PER_HASH_LATENCY_NS
}

/// The smallest power-of-two chunk_blocks whose gamma is at least
/// `margin` times q.  None if even the graph's maximum is not enough.
/// (The inverse of delta_ms_for; kept for the tests and for reasoning.)
#[cfg_attr(not(test), allow(dead_code))]
pub fn chunk_blocks_for(delta_ms: u64, margin: f64) -> Option<u32> {
    let need = margin * q(delta_ms);
    let mut cb = 1u32;
    loop {
        if ffi::gamma(cb) as f64 >= need {
            return Some(cb);
        }
        if cb >= ffi::POSE_GRAPH_MAX_CHUNK_BLOCKS {
            return None;
        }
        cb <<= 1;
    }
}

// ── targets and keys ──────────────────────────────────────────────────────

/// How a target is named to people and in the UI form: node-qualified, with
/// the base for RAM (all RAM regions are called "ram").
pub fn target_label(node: &str, name: &str, base: u64, is_ram: bool) -> String {
    let n = if is_ram { format!("{name}@{base:#x}") } else { name.to_string() };
    if node.is_empty() { n } else { format!("{node}:{n}") }
}

pub fn entry_label(e: &Entry) -> String {
    target_label(&e.node, &e.name, e.base, e.kind == Kind::Ram)
}

/// Nesting depth of a node: "" 0, "dpu0" 1, "dpu0/bmc0" 2.
fn depth(node: &str) -> usize {
    if node.is_empty() { 0 } else { node.matches('/').count() + 1 }
}

/// Within a node: disks, then RAM, then GPUs.
fn kind_order(k: Kind) -> u8 {
    match k {
        Kind::Disk => 0,
        Kind::Gpu => 2,
        _ => 1,
    }
}

/// The algorithm a plan's non-GPU sessions use: BLAKE3 when the source has
/// a GPU to generate them on, AES-PRF when it generates on its CPU.
pub fn algo_for(inv: &[Entry]) -> Algo {
    if inv.iter().any(|e| e.node.is_empty() && e.kind == Kind::Gpu) { Algo::Blake3 } else { Algo::AesPrf }
}

/// The targets of an inventory in labelling order.
pub fn ordered_targets(inv: &[Entry]) -> Vec<&Entry> {
    let mut t: Vec<&Entry> = inv.iter().filter(|e| e.kind.is_target()).collect();
    t.sort_by(|a, b| {
        depth(&b.node)
            .cmp(&depth(&a.node))
            .then_with(|| a.node.cmp(&b.node))
            .then_with(|| kind_order(a.kind).cmp(&kind_order(b.kind)))
            .then_with(|| a.base.cmp(&b.base))
            .then_with(|| a.name.cmp(&b.name))
    });
    t
}

/// A target the plan leaves out, and why.
#[derive(Clone, Debug, Serialize)]
pub struct Skipped {
    pub target: String,
    pub reason: String,
}

/// The default plan over every target `selected` accepts (by entry_label).
pub fn default_plan(
    inv: &[Entry],
    p: &Params,
    selected: impl Fn(&str) -> bool,
) -> Result<(Plan, Vec<Skipped>)> {
    let cb = p.chunk_blocks;
    if cb == 0 || !cb.is_power_of_two() || cb > ffi::POSE_GRAPH_MAX_CHUNK_BLOCKS {
        bail!("chunk_blocks {cb} is not a power of two in [1, {}]", ffi::POSE_GRAPH_MAX_CHUNK_BLOCKS);
    }
    let unit = cb as u64 * LABEL_BYTES as u64;
    let algo = algo_for(inv);
    let mut plan = Plan { finish: p.finish, sessions: vec![] };
    let mut skipped = vec![];
    for e in ordered_targets(inv) {
        let label = entry_label(e);
        if !selected(&label) {
            skipped.push(Skipped { target: label, reason: "not selected".into() });
            continue;
        }
        let usable = usable_size(e, cb);
        let size = usable - usable % unit;
        if size == 0 {
            let reason = if e.kind == Kind::Gpu && usable < unit {
                format!(
                    "no room for a super-chunk ({} MiB) beside the GPU labeler's reserve ({} MiB: topology, margin, one pool)",
                    unit >> 20,
                    gpu_reserve(cb) >> 20
                )
            } else {
                format!("smaller than one super-chunk ({} MiB)", unit >> 20)
            };
            skipped.push(Skipped { target: label, reason });
            continue;
        }
        plan.sessions.push(Session {
            node: e.node.clone(),
            target: e.name.clone(),
            base: e.base,
            size,
            chunk_blocks: cb,
            // The GPU labeler implements BLAKE3 only.
            algo: if e.kind == Kind::Gpu { Algo::Blake3 } else { algo },
            challenges: p.challenges,
            delta_ms: p.delta_ms,
        });
    }
    Ok((plan, skipped))
}

// ── validation ────────────────────────────────────────────────────────────

/// What `validate` found: problems block a run, notes are for the record.
#[derive(Clone, Debug, Default, Serialize)]
pub struct Validation {
    pub problems: Vec<String>,
    pub notes: Vec<String>,
}

/// Check a plan against the live inventory and the security bounds.
pub fn validate(plan: &Plan, inv: &[Entry]) -> Validation {
    let mut v = Validation::default();
    let n = plan.sessions.len() as u64;
    if n == 0 {
        v.problems.push("the plan has no sessions".into());
    }
    if n > c::SINK_MAX_SESSIONS {
        v.problems.push(format!("{n} sessions, sink limit {}: untick some targets", c::SINK_MAX_SESSIONS));
    }
    for (i, s) in plan.sessions.iter().enumerate() {
        let who = format!("session {i} ({})", s.label());
        let cb = s.chunk_blocks;
        if cb == 0 || !cb.is_power_of_two() || cb > ffi::POSE_GRAPH_MAX_CHUNK_BLOCKS {
            v.problems.push(format!("{who}: chunk_blocks {cb} is not a power of two in [1, {}]", ffi::POSE_GRAPH_MAX_CHUNK_BLOCKS));
            continue;
        }
        let unit = cb as u64 * LABEL_BYTES as u64;
        if s.size == 0 || s.size % unit != 0 {
            v.problems.push(format!("{who}: size {} is not a whole number of {unit}-byte super-chunks", s.size));
        }
        let entry = inv.iter().find(|e| {
            e.kind.is_target()
                && e.node == s.node
                && e.name == s.target
                && s.base >= e.base
                && s.size <= e.size
                && s.base - e.base <= e.size - s.size
        });
        match entry {
            None => v.problems.push(format!("{who}: not inside any target of the inventory")),
            Some(e) if e.kind == Kind::Gpu => {
                // What the source-daemon checks before allocating (gpu_sessions.c).
                if s.algo != Algo::Blake3 {
                    v.problems.push(format!("{who}: GPU sessions label with BLAKE3 only, not {}", s.algo.name()));
                }
                if s.base != 0 {
                    v.problems.push(format!("{who}: a GPU session starts at base 0"));
                }
                let floor = gpu_reserve(cb);
                if s.size + floor > e.size {
                    v.problems.push(format!(
                        "{who}: size {} MiB leaves less than the {} MiB the GPU labeler needs beside it (of {} MiB labelable)",
                        s.size >> 20,
                        floor >> 20,
                        e.size >> 20
                    ));
                }
            }
            Some(_) => {}
        }
        for (j, t) in plan.sessions[..i].iter().enumerate() {
            let same_space = t.node == s.node && (s.target == "ram") == (t.target == "ram") && (s.target == "ram" || t.target == s.target);
            if same_space && s.base < t.base + t.size && t.base < s.base + s.size {
                v.problems.push(format!("{who}: overlaps session {j}"));
            }
        }
        if s.challenges == 0 {
            v.problems.push(format!("{who}: no challenges"));
        }

        // Distance bounding: a relay must chain gamma hashes in one round trip.
        let (g, q) = (ffi::gamma(cb), q(s.delta_ms));
        if g as f64 <= q {
            v.problems.push(format!(
                "{who}: insecure: gamma {g} (chunk_blocks {cb}) <= q {q:.0} ({} ms at {PER_HASH_LATENCY_NS} ns/hash)",
                s.delta_ms
            ));
        }
        // Soundness: the chance a prover that freed ASSUMED_FREE_FRACTION of
        // the labels survives every challenge.
        let bits = s.challenges as f64 * -(1.0 - ASSUMED_FREE_FRACTION).log2();
        v.notes.push(format!(
            "{who}: gamma {g} vs q {q:.0} ({:.2}x); {} challenges detect a prover that freed {:.0}% of the labels with probability {:.4} ({bits:.1} bits)",
            g as f64 / q,
            s.challenges,
            ASSUMED_FREE_FRACTION * 100.0,
            1.0 - (1.0 - ASSUMED_FREE_FRACTION).powi(s.challenges as i32),
        ));
    }
    v
}

// ── TOML ──────────────────────────────────────────────────────────────────

pub fn from_toml(text: &str) -> Result<Plan> {
    let plan: Plan = toml::from_str(text).context("parsing the plan")?;
    if plan.sessions.is_empty() {
        bail!("the plan has no [[session]] entries");
    }
    Ok(plan)
}

/// The plan as TOML, hand-formatted so addresses are hex and sizes readable.
pub fn to_toml(plan: &Plan, header: &str) -> String {
    let mut t = String::new();
    for line in header.lines() {
        writeln!(t, "# {line}").unwrap();
    }
    writeln!(t, "#\n# Seeds are not recorded: every run draws fresh ones.\n").unwrap();
    writeln!(t, "finish = \"{}\"", plan.finish.name()).unwrap();
    for (i, s) in plan.sessions.iter().enumerate() {
        writeln!(t, "\n[[session]]   # {i}: {}, {} MiB", s.label(), s.size >> 20).unwrap();
        writeln!(t, "node = {:?}", s.node).unwrap();
        writeln!(t, "target = {:?}", s.target).unwrap();
        writeln!(t, "base = {:#x}", s.base).unwrap();
        writeln!(t, "size = {:#x}", s.size).unwrap();
        writeln!(t, "chunk_blocks = {}", s.chunk_blocks).unwrap();
        writeln!(t, "algo = \"{}\"", s.algo.name()).unwrap();
        writeln!(t, "challenges = {}", s.challenges).unwrap();
        writeln!(t, "delta_ms = {}", s.delta_ms).unwrap();
    }
    t
}

#[cfg(test)]
mod tests {
    use super::*;

    fn e(kind: Kind, node: &str, name: &str, base: u64, size: u64) -> Entry {
        Entry { kind, node: node.into(), name: name.into(), base, size, sector: if kind == Kind::Disk { 512 } else { 0 } }
    }

    fn inv() -> Vec<Entry> {
        vec![
            e(Kind::Ram, "", "ram", 0x2c000_0000, 1865 << 20),
            e(Kind::Ram, "", "ram", 0x3_3766_2000, (73 << 20) + 0x1000),
            e(Kind::Disk, "", "/dev/vda", 0, 32 << 30),
            e(Kind::Kernel, "", "System RAM", 0x4000_0000, 10 << 30),
            e(Kind::Ram, "dpu0", "ram", 0x8000_0000, 4 << 30),
            e(Kind::Disk, "dpu0", "/dev/nvme0n1", 0, 10 << 30),
            e(Kind::Ram, "dpu0/bmc0", "ram", 0x9000_0000, 256 << 20),
            e(Kind::Ram, "", "ram", 0x10_0000_0000, 2 << 20), // smaller than a super-chunk
            e(Kind::Gpu, "", "gpu0", 0, 142119 << 20),
            e(Kind::GpuDeclared, "", "gpu0", 0, 143157 << 20),
            e(Kind::GpuReserved, "", "headroom", 142119 << 20, 512 << 20),
            e(Kind::Gpu, "", "gpu1", 0, 256 << 20), // too small beside the scratch
        ]
    }

    #[test]
    fn defaults_are_2_17_at_6_ms() {
        // gamma(2^17) = 2^16 = 65,536; 1.5 q(6 ms) = 60,000 fits, 1.5 q(7 ms) = 70,000 does not.
        let p = Params::default();
        assert_eq!((p.chunk_blocks, p.delta_ms), (1 << 17, 6));
        assert_eq!(ffi::gamma(1 << 17), 1 << 16);
        assert_eq!(chunk_blocks_for(6, DEFAULT_MARGIN), Some(1 << 17));
        assert_eq!(chunk_blocks_for(10, DEFAULT_MARGIN), Some(1 << 18));
    }

    #[test]
    fn default_plan_order_and_rounding() {
        let (plan, skipped) = default_plan(&inv(), &Params::default(), |_| true).unwrap();
        let labels: Vec<String> = plan.sessions.iter().map(|s| s.label()).collect();
        assert_eq!(
            labels,
            [
                "dpu0/bmc0:ram@0x90000000",
                "dpu0:/dev/nvme0n1",
                "dpu0:ram@0x80000000",
                "/dev/vda",
                "ram@0x2c0000000",
                "ram@0x337662000",
                "gpu0",
            ]
        );
        let unit = (1u64 << 17) * 32; // 4 MiB
        assert!(plan.sessions.iter().all(|s| s.size % unit == 0 && s.size > 0));
        assert_eq!(plan.sessions[4].size, 1864 << 20); // 1865 MiB rounded down to 4 MiB
        assert_eq!(plan.sessions[5].size, 72 << 20);
        // The source has a GPU: everything is BLAKE3 so it can generate there.
        assert!(plan.sessions.iter().all(|s| s.algo == Algo::Blake3));
        let no_gpu: Vec<Entry> = inv().into_iter().filter(|e| e.kind != Kind::Gpu).collect();
        let (plan3, _) = default_plan(&no_gpu, &Params::default(), |_| true).unwrap();
        assert!(plan3.sessions.iter().all(|s| s.algo == Algo::AesPrf));
        // The GPU session: BLAKE3, base 0, only the labeler's reserve left free
        // (topology + margin + one pool: ~1.8 GiB at 2^18, not the 17.8 GiB a
        // 1024-pool allocation would be — the session uses its own tail).
        let g = &plan.sessions[6];
        assert_eq!((g.algo, g.base), (Algo::Blake3, 0));
        let reserve = gpu_reserve(1 << 17);
        assert!(reserve > (512u64 << 20) && reserve < (1536u64 << 20), "reserve {} MiB", reserve >> 20);
        assert!(g.size + reserve <= 142119 << 20 && g.size + reserve + unit > 142119 << 20);
        assert_eq!(skipped.len(), 2);
        let why = |t: &str| skipped.iter().find(|s| s.target == t).map(|s| s.reason.clone()).unwrap_or_default();
        assert!(why("ram@0x1000000000").contains("smaller than one super-chunk"));
        assert!(why("gpu1").contains("beside the GPU labeler's reserve"));
        let v = validate(&plan, &inv());
        assert!(v.problems.is_empty(), "{:?}", v.problems);
    }

    #[test]
    fn selection_and_validation() {
        let (plan, skipped) = default_plan(&inv(), &Params::default(), |l| l.starts_with("dpu0:")).unwrap();
        assert_eq!(plan.sessions.len(), 2);
        assert_eq!(skipped.iter().filter(|s| s.reason == "not selected").count(), 7);

        let mut bad = plan.clone();
        bad.sessions[0].delta_ms = 100; // q = 666,667 > gamma
        bad.sessions[1].base += 8 << 20; // past the end of the disk
        bad.sessions.push(bad.sessions[0].clone()); // overlap
        let v = validate(&bad, &inv());
        assert!(v.problems.iter().any(|p| p.contains("insecure")));
        assert!(v.problems.iter().any(|p| p.contains("not inside")));
        assert!(v.problems.iter().any(|p| p.contains("overlaps session 0")));
    }

    #[test]
    fn gpu_session_checks() {
        let (plan, _) = default_plan(&inv(), &Params::default(), |l| l == "gpu0").unwrap();
        assert_eq!(plan.sessions.len(), 1);
        assert!(validate(&plan, &inv()).problems.is_empty());

        let mut bad = plan.clone();
        bad.sessions[0].algo = Algo::AesPrf;
        bad.sessions[0].size = 142119 << 20; // the whole labelable HBM: nothing left for scratch
        let v = validate(&bad, &inv());
        assert!(v.problems.iter().any(|p| p.contains("BLAKE3 only")));
        assert!(v.problems.iter().any(|p| p.contains("GPU labeler needs beside it")));
        let mut off = plan.clone();
        off.sessions[0].base = 8 << 20; // still inside the target, but not where the daemon labels
        let v = validate(&off, &inv());
        assert!(v.problems.iter().any(|p| p.contains("base 0")), "{:?}", v.problems);
    }

    #[test]
    fn session_limit() {
        let many: Vec<Entry> = (0..40).map(|i| e(Kind::Disk, "", &format!("/dev/sd{i}"), 0, 1 << 30)).collect();
        let (plan, _) = default_plan(&many, &Params::default(), |_| true).unwrap();
        let v = validate(&plan, &many);
        assert!(v.problems.iter().any(|p| p.contains("sink limit")));
    }

    #[test]
    fn toml_round_trip() {
        let (plan, _) = default_plan(&inv(), &Params::default(), |_| true).unwrap();
        let text = to_toml(&plan, "test plan");
        assert!(text.contains("base = 0x2c0000000"));
        assert!(!text.contains("seed"));
        assert_eq!(from_toml(&text).unwrap(), plan);
    }
}
