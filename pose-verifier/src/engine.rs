//! One verification run against a source-daemon:
//!
//!   1. InventoryRequest / Inventory on a fresh connection; the plan is
//!      validated against that inventory and the security bounds (nothing is
//!      sent if it fails, so the tray holds nothing and can be asked again).
//!   2. Per session, in plan order: SessionInit with a fresh seed, InitAck,
//!      Progress frames while the source generates, Coverage.  The challenge
//!      indices are drawn as soon as the session is accepted and their
//!      expected labels computed in the background meanwhile (they depend
//!      only on the seed, which the verifier chose).
//!   3. Every challenge of every session in one shuffled order, each timed
//!      against its session's delta_ms.
//!   4. Finish with the plan's action, whatever the verdict.
//!
//! A session passes when every challenge returned the expected label within
//! delta_ms; the run passes when every session does.  A rejected InitAck, an
//! Error frame, a lost connection or a Coverage that is not exactly the
//! session's range fails the run at once: nothing more is started or
//! challenged, and Finish is sent if the connection still works.
//!
//! Everything the run learns goes into a RunStatus behind a mutex, which is
//! both the JSON record written at every phase change and what a UI shows.

use crate::labels;
use crate::plan::{self, Plan, Validation};
use crate::proto::{self, c, Conn, Entry, Label, Reply, Seed};
use anyhow::{anyhow, bail, ensure, Result};
use rand::seq::SliceRandom;
use rand::{Rng, RngCore};
use serde::Serialize;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Phase {
    /// Sessions being set up and labelled.
    Labelling,
    /// Waiting for the verifier's own expected labels.
    Expecting,
    Challenging,
    /// Every session passed.
    Passed,
    /// Ran to the end; at least one session failed its challenges.
    Rejected,
    /// Stopped early: see `reason`.
    Failed,
}

impl Phase {
    pub fn is_final(self) -> bool {
        matches!(self, Phase::Passed | Phase::Rejected | Phase::Failed)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum SessionState {
    Planned,
    Generating,
    Stored,
    Passed,
    Failed,
}

#[derive(Clone, Debug, Default, Serialize)]
pub struct Rtt {
    pub min_ms: f64,
    pub avg_ms: f64,
    pub max_ms: f64,
}

#[derive(Clone, Debug, Serialize)]
pub struct SessionStatus {
    pub number: u32,
    pub plan: plan::Session,
    pub state: SessionState,
    /// Hex; drawn when the session is initialised.
    pub seed: Option<String>,
    pub bytes_done: u64,
    /// Bytes per second, EWMA over Progress frames (see on_progress).
    pub rate: Option<f64>,
    pub eta_s: Option<f64>,
    pub generate_s: Option<f64>,
    pub coverage: Vec<(u64, u64)>,
    /// Chunks the verifier labels itself to check this session's challenges.
    pub expected_chunks: usize,
    pub expected_done: usize,
    pub challenges_ok: u32,
    pub challenges_bad: u32,
    /// Session block indices whose label was wrong.
    pub failed_blocks: Vec<u64>,
    pub rtt: Option<Rtt>,
    pub rtt_violations: u32,
    pub verdict: Option<String>,
    #[serde(skip)]
    rtt_sum: f64,
    #[serde(skip)]
    last_progress: Option<(Instant, u64)>,
}

#[derive(Clone, Debug, Serialize)]
pub struct RunStatus {
    pub record_version: u32,
    pub source: String,
    pub started: u64,
    pub ended: Option<u64>,
    pub phase: Phase,
    pub reason: Option<String>,
    pub finish: plan::Finish,
    pub finish_sent: Option<String>,
    pub validation: Validation,
    pub sessions: Vec<SessionStatus>,
    pub inventory: Vec<Entry>,
}

fn unix_now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Where a run's record and reproducible plan go.
pub struct Recorder {
    pub json: PathBuf,
    pub toml: PathBuf,
}

impl Recorder {
    pub fn new(dir: &std::path::Path, started: u64) -> Result<Recorder> {
        std::fs::create_dir_all(dir)?;
        Ok(Recorder { json: dir.join(format!("run-{started}.json")), toml: dir.join(format!("run-{started}.toml")) })
    }

    fn write(&self, st: &RunStatus) {
        let tmp = self.json.with_extension("json.tmp");
        let res = serde_json::to_vec_pretty(st)
            .map_err(anyhow::Error::from)
            .and_then(|b| Ok(std::fs::write(&tmp, b)?))
            .and_then(|_| Ok(std::fs::rename(&tmp, &self.json)?));
        if let Err(e) = res {
            eprintln!("warning: cannot write {}: {e:#}", self.json.display());
        }
    }
}

/// Fetch the inventory on a new connection.  The connection is returned so a
/// run can carry on with it; dropping it leaves the tray holding nothing.
pub fn inventory(addr: &str) -> Result<(Conn, Vec<Entry>)> {
    let mut conn = Conn::connect(addr)?;
    conn.send(c::SINK_MSG_INVENTORY_REQUEST, &proto::enc_inventory_request(c::SINK_PROTO_VERSION as u16))?;
    match conn.recv()? {
        Reply::Inventory(v) => Ok((conn, v)),
        Reply::Error(e) => bail!("source error: {e}"),
        r => bail!("unexpected reply to InventoryRequest: {r:?}"),
    }
}

/// A run in progress or finished.
pub struct Run {
    pub status: Arc<Mutex<RunStatus>>,
    rec: Recorder,
    log: Box<dyn Fn(&str) + Send>,
    /// Labelling threads for the expected labels.
    pub threads: usize,
}

struct ExpectJob {
    number: u32,
    seed: Seed,
    session: plan::Session,
    indices: Vec<u64>,
}

/// Per session: the drawn challenge indices (in draw order, repeats kept)
/// and the expected label of each.
type Challenges = (Vec<u64>, HashMap<u64, Label>);
type Expected = HashMap<u32, Result<Challenges>>;

impl Run {
    /// Validate `plan` against `inventory` and prepare the record.  Returns
    /// the Run and whether it may start (validation problems are in the
    /// status).
    pub fn new(
        source: &str,
        plan: &Plan,
        inventory: Vec<Entry>,
        out_dir: &std::path::Path,
        log: Box<dyn Fn(&str) + Send>,
    ) -> Result<(Run, bool)> {
        let validation = plan::validate(plan, &inventory);
        let ok = validation.problems.is_empty();
        let started = unix_now();
        let sessions = plan
            .sessions
            .iter()
            .enumerate()
            .map(|(i, s)| SessionStatus {
                number: i as u32,
                plan: s.clone(),
                state: SessionState::Planned,
                seed: None,
                bytes_done: 0,
                rate: None,
                eta_s: None,
                generate_s: None,
                coverage: vec![],
                expected_chunks: 0,
                expected_done: 0,
                challenges_ok: 0,
                challenges_bad: 0,
                failed_blocks: vec![],
                rtt: None,
                rtt_violations: 0,
                verdict: None,
                rtt_sum: 0.0,
                last_progress: None,
            })
            .collect();
        let status = RunStatus {
            record_version: 1,
            source: source.to_string(),
            started,
            ended: None,
            phase: if ok { Phase::Labelling } else { Phase::Failed },
            reason: if ok { None } else { Some(format!("plan rejected: {}", validation.problems.join("; "))) },
            finish: plan.finish,
            finish_sent: None,
            validation,
            sessions,
            inventory,
        };
        let rec = Recorder::new(out_dir, started)?;
        let header = format!(
            "pose-verifier run {started} against {source}\nRepeat with: pose-verifier run {source} --plan {}",
            rec.toml.display()
        );
        std::fs::write(&rec.toml, plan::to_toml(plan, &header))?;
        let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
        let run = Run { status: Arc::new(Mutex::new(status)), rec, log, threads };
        run.save();
        Ok((run, ok))
    }

    pub fn record_paths(&self) -> (&std::path::Path, &std::path::Path) {
        (&self.rec.json, &self.rec.toml)
    }

    fn save(&self) {
        self.rec.write(&self.status.lock().unwrap());
    }

    fn log(&self, msg: &str) {
        (self.log)(msg);
    }

    fn with<R>(&self, f: impl FnOnce(&mut RunStatus) -> R) -> R {
        f(&mut self.status.lock().unwrap())
    }

    /// Execute the run on `conn` (from `inventory`).  Returns the final phase.
    pub fn execute(&self, mut conn: Conn) -> Phase {
        let outcome = self.drive(&mut conn);
        let (phase, reason) = match outcome {
            Ok(()) => {
                let failed: Vec<u32> = self.with(|st| {
                    st.sessions.iter().filter(|s| s.state != SessionState::Passed).map(|s| s.number).collect()
                });
                if failed.is_empty() {
                    (Phase::Passed, "proof accepted".to_string())
                } else {
                    let list: Vec<String> = failed.iter().map(|n| n.to_string()).collect();
                    (Phase::Rejected, format!("proof rejected: session(s) {} failed", list.join(", ")))
                }
            }
            Err(e) => (Phase::Failed, format!("run failed: {e:#}")),
        };
        self.log(&reason);
        let action = self.with(|st| st.finish);
        let sent = proto::enc_finish(action.wire(), &reason)
            .and_then(|p| conn.send(c::SINK_MSG_FINISH, &p))
            .map(|_| format!("{} ({reason})", action.name()));
        match &sent {
            Ok(s) => self.log(&format!("Finish sent: {s}")),
            Err(e) => self.log(&format!("Finish not sent ({e:#}); the tray takes its fail-safe")),
        }
        self.with(|st| {
            st.phase = phase;
            st.reason = Some(reason);
            st.finish_sent = sent.ok();
            st.ended = Some(unix_now());
        });
        self.save();
        phase
    }

    fn drive(&self, conn: &mut Conn) -> Result<()> {
        let (job_tx, results) = self.start_expect_worker();
        let n = self.with(|st| st.sessions.len());
        for i in 0..n {
            self.label_session(conn, i, &job_tx)?;
            self.save();
        }
        drop(job_tx);

        self.with(|st| st.phase = Phase::Expecting);
        self.save();
        self.log("computing expected labels");
        let expected = results.join().map_err(|_| anyhow!("expected-label worker panicked"))?;
        let mut expected_by: HashMap<u32, Challenges> = HashMap::new();
        for (num, r) in expected {
            expected_by.insert(num, r?);
        }

        self.with(|st| st.phase = Phase::Challenging);
        self.save();
        self.challenge(conn, &expected_by)?;
        Ok(())
    }

    /// SessionInit .. Coverage for session i.  Its expected-label job is
    /// queued as soon as the session is accepted.
    fn label_session(&self, conn: &mut Conn, i: usize, jobs: &mpsc::Sender<ExpectJob>) -> Result<()> {
        let mut seed: Seed = [0; proto::SEED_BYTES];
        rand::rngs::OsRng.fill_bytes(&mut seed);
        let s = self.with(|st| {
            let ss = &mut st.sessions[i];
            ss.seed = Some(hex::encode(seed));
            ss.plan.clone()
        });
        let num = i as u32;
        let init = proto::SessionInit {
            session: num,
            node: s.node.clone(),
            target: s.target.clone(),
            base: s.base,
            size: s.size,
            chunk_blocks: s.chunk_blocks,
            seed,
            algo: s.algo.wire(),
        };
        conn.send(c::SINK_MSG_SESSION_INIT, &proto::enc_session_init(&init)?)?;
        match conn.recv()? {
            Reply::InitAck { ok: true, .. } => {}
            Reply::InitAck { ok: false, error } => bail!("session {num} ({}) rejected: {error}", s.label()),
            Reply::Error(e) => bail!("source error: {e}"),
            r => bail!("unexpected reply to SessionInit: {r:?}"),
        }
        self.log(&format!("session {num}: {} {} MiB accepted, generating", s.label(), s.size >> 20));

        // Draw the challenges now, so the verifier labels them while the
        // source generates.  Uniform over the session's blocks.
        let mut rng = rand::thread_rng();
        let indices: Vec<u64> = (0..s.challenges).map(|_| rng.gen_range(0..s.blocks())).collect();
        let chunks = labels::chunk_count(s.base, s.chunk_blocks, &indices);
        jobs.send(ExpectJob { number: num, seed, session: s.clone(), indices })
            .map_err(|_| anyhow!("expected-label worker stopped"))?;

        let t0 = Instant::now();
        self.with(|st| {
            let ss = &mut st.sessions[i];
            ss.state = SessionState::Generating;
            ss.expected_chunks = chunks;
        });
        let mut last_log = t0;
        loop {
            match conn.recv()? {
                Reply::Progress { session, bytes_done } => {
                    ensure!(session == num, "Progress for session {session} while session {num} generates");
                    let line = self.on_progress(i, bytes_done);
                    if last_log.elapsed() >= Duration::from_secs(10) {
                        self.log(&line);
                        last_log = Instant::now();
                    }
                }
                Reply::Coverage(runs) => {
                    let want = vec![(s.base, s.blocks())];
                    ensure!(runs == want, "session {num}: Coverage {runs:x?} does not match its range {want:x?}");
                    let secs = t0.elapsed().as_secs_f64();
                    self.with(|st| {
                        let ss = &mut st.sessions[i];
                        ss.state = SessionState::Stored;
                        ss.bytes_done = s.size;
                        ss.eta_s = None;
                        ss.generate_s = Some(secs);
                        ss.coverage = runs;
                    });
                    self.log(&format!(
                        "session {num}: stored {} MiB in {secs:.1} s ({:.1} MiB/s)",
                        s.size >> 20,
                        s.size as f64 / (1 << 20) as f64 / secs.max(1e-9)
                    ));
                    return Ok(());
                }
                Reply::Error(e) => bail!("source error: {e}"),
                r => bail!("unexpected frame while session {num} generates: {r:?}"),
            }
        }
    }

    /// Update rate and ETA from a Progress frame; returns a log line.  The
    /// source delivers labels a wave of super-chunks at a time, so frames come
    /// in bursts: each interval's rate is weighted by its length (time
    /// constant TAU_S), so a long gap between waves counts for more than the
    /// short ones inside a burst.  The first frame only sets the starting
    /// point: the time before it includes the source's start-up.
    fn on_progress(&self, i: usize, done: u64) -> String {
        const TAU_S: f64 = 10.0;
        let now = Instant::now();
        self.with(|st| {
            let ss = &mut st.sessions[i];
            if let Some((t, d)) = ss.last_progress {
                let dt = now.duration_since(t).as_secs_f64();
                if dt > 0.0 && done >= d {
                    let inst = (done - d) as f64 / dt;
                    let alpha = 1.0 - (-dt / TAU_S).exp();
                    ss.rate = Some(ss.rate.map_or(inst, |r| alpha * inst + (1.0 - alpha) * r));
                }
            }
            ss.last_progress = Some((now, done));
            ss.bytes_done = done;
            ss.eta_s = ss.rate.filter(|&r| r > 0.0).map(|r| (ss.plan.size.saturating_sub(done)) as f64 / r);
            format!(
                "session {}: {} / {} MiB ({:.0}%), {:.1} MiB/s, ETA {}",
                ss.number,
                done >> 20,
                ss.plan.size >> 20,
                100.0 * done as f64 / ss.plan.size as f64,
                ss.rate.unwrap_or(0.0) / (1 << 20) as f64,
                ss.eta_s.map_or("?".into(), fmt_duration),
            )
        })
    }

    /// The background worker that labels each accepted session's challenged
    /// chunks, one session at a time, each across all threads.
    fn start_expect_worker(&self) -> (mpsc::Sender<ExpectJob>, std::thread::JoinHandle<Expected>) {
        let (tx, rx) = mpsc::channel::<ExpectJob>();
        let status = self.status.clone();
        let threads = self.threads;
        let h = std::thread::spawn(move || {
            let mut out = Expected::new();
            for job in rx {
                let done = AtomicUsize::new(0);
                let s = &job.session;
                let r = std::thread::scope(|sc| {
                    let worker = sc.spawn(|| {
                        labels::expected(&job.seed, s.algo.wire(), s.chunk_blocks, s.base, &job.indices, threads, &done)
                    });
                    while !worker.is_finished() {
                        status.lock().unwrap().sessions[job.number as usize].expected_done = done.load(Ordering::Relaxed);
                        std::thread::sleep(Duration::from_millis(250));
                    }
                    worker.join().unwrap_or_else(|_| Err(anyhow!("labelling panicked")))
                });
                status.lock().unwrap().sessions[job.number as usize].expected_done = done.load(Ordering::Relaxed);
                out.insert(job.number, r.map(|m| (job.indices, m)));
            }
            out
        });
        (tx, h)
    }

    /// Every challenge of every session, shuffled, each timed.
    fn challenge(&self, conn: &mut Conn, expected: &HashMap<u32, Challenges>) -> Result<()> {
        let mut order: Vec<(u32, u64)> = vec![];
        for (&num, (indices, _)) in expected {
            order.extend(indices.iter().map(|&i| (num, i)));
        }
        order.shuffle(&mut rand::thread_rng());
        self.log(&format!("challenging: {} challenges across {} session(s)", order.len(), expected.len()));

        for (num, index) in order {
            let delta = self.with(|st| Duration::from_millis(st.sessions[num as usize].plan.delta_ms));
            let payload = proto::enc_challenge(num, index);
            let t0 = Instant::now();
            conn.send(c::SINK_MSG_CHALLENGE, &payload)?;
            let reply = conn.recv()?;
            let rtt = t0.elapsed();
            let label = match reply {
                Reply::Label(l) => l,
                Reply::Error(e) => bail!("source error: {e}"),
                r => bail!("unexpected reply to Challenge: {r:?}"),
            };
            let good = expected[&num].1.get(&index) == Some(&label);
            self.with(|st| {
                let ss = &mut st.sessions[num as usize];
                if good {
                    ss.challenges_ok += 1;
                } else {
                    ss.challenges_bad += 1;
                    ss.failed_blocks.push(index);
                }
                let ms = rtt.as_secs_f64() * 1e3;
                let n = (ss.challenges_ok + ss.challenges_bad) as f64;
                ss.rtt_sum += ms;
                let r = ss.rtt.get_or_insert(Rtt { min_ms: ms, avg_ms: ms, max_ms: ms });
                r.min_ms = r.min_ms.min(ms);
                r.max_ms = r.max_ms.max(ms);
                r.avg_ms = ss.rtt_sum / n;
                if rtt > delta {
                    ss.rtt_violations += 1;
                }
            });
        }

        self.with(|st| {
            for ss in &mut st.sessions {
                let total = ss.challenges_ok + ss.challenges_bad;
                let mut why = vec![];
                if ss.challenges_bad > 0 {
                    why.push(format!("{}/{total} labels wrong", ss.challenges_bad));
                }
                if ss.rtt_violations > 0 {
                    why.push(format!("{}/{total} round trips over {} ms", ss.rtt_violations, ss.plan.delta_ms));
                }
                ss.failed_blocks.sort_unstable();
                if why.is_empty() {
                    ss.state = SessionState::Passed;
                    ss.verdict = Some(format!("{total}/{total} labels correct within {} ms", ss.plan.delta_ms));
                } else {
                    ss.state = SessionState::Failed;
                    ss.verdict = Some(why.join(", "));
                }
            }
        });
        let lines: Vec<String> = self.with(|st| {
            st.sessions
                .iter()
                .map(|ss| {
                    let r = ss.rtt.clone().unwrap_or_default();
                    format!(
                        "session {} ({}): {} — {} (RTT min/avg/max {:.2}/{:.2}/{:.2} ms)",
                        ss.number,
                        ss.plan.label(),
                        if ss.state == SessionState::Passed { "PASS" } else { "FAIL" },
                        ss.verdict.as_deref().unwrap_or(""),
                        r.min_ms,
                        r.avg_ms,
                        r.max_ms
                    )
                })
                .collect()
        });
        for l in lines {
            self.log(&l);
        }
        Ok(())
    }
}

pub fn fmt_duration(s: f64) -> String {
    let s = s.max(0.0).round() as u64;
    match s {
        0..=59 => format!("{s}s"),
        60..=3599 => format!("{}m{:02}s", s / 60, s % 60),
        _ => format!("{}h{:02}m", s / 3600, s / 60 % 60),
    }
}
