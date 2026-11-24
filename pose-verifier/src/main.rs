//! pose-verifier — drives a PoSE-DB erasure of a whole tray through its
//! source-daemon (pose-sink/include/sink_proto.h) and checks the proof.
//!
//!   pose-verifier inventory HOST[:PORT]          what the tray can label, and where its RAM went
//!   pose-verifier plan HOST[:PORT] [-o FILE]     the default plan (every target, full extent) as TOML
//!   pose-verifier run HOST[:PORT] [--plan FILE]  label, challenge, Finish; writes run-<time>.{json,toml}
//!   pose-verifier serve HOST[:PORT]              the same from a web page on port 8080
//!
//! A run step by step, and the protocol it speaks, are explained in
//! docs/sink-source-architecture.md.

mod engine;
mod ffi;
mod labels;
mod plan;
mod proto;
mod web;

use anyhow::{bail, Result};
use clap::{Parser, Subcommand};
use proto::Kind;
use std::path::PathBuf;

#[derive(Parser)]
#[command(about = "PoSE-DB tray verifier")]
struct Cli {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand)]
enum Cmd {
    /// Print the tray's inventory: targets, and each node's RAM map.
    Inventory {
        /// The source-daemon, HOST[:PORT] (port defaults to 7776).
        source: String,
    },
    /// Print the default plan for the tray's current inventory as TOML.
    Plan {
        source: String,
        /// Write it to this file instead of stdout.
        #[arg(short, long)]
        output: Option<PathBuf>,
        /// The Finish action the plan carries.
        #[arg(long, value_enum, default_value = "halt")]
        finish: plan::Finish,
        /// Leave a target out, as printed by `inventory`-style labels
        /// ("dpu0:ram@0x80000000", "/dev/vda", "gpu0"); repeatable.
        #[arg(long)]
        exclude: Vec<String>,
    },
    /// Run a verification: the default plan, or --plan.
    Run {
        source: String,
        /// A plan TOML (e.g. a previous run's run-<time>.toml).
        #[arg(long)]
        plan: Option<PathBuf>,
        /// Override the plan's Finish action.
        #[arg(long, value_enum)]
        finish: Option<plan::Finish>,
        /// Without --plan: leave a target out of the default plan; repeatable.
        #[arg(long)]
        exclude: Vec<String>,
        /// Where run-<time>.json and run-<time>.toml go.
        #[arg(long, default_value = "runs")]
        out_dir: PathBuf,
        /// Threads for the verifier's own label computation (default: all CPUs).
        #[arg(long)]
        threads: Option<usize>,
        /// Validate and print the plan, send nothing.
        #[arg(long)]
        dry_run: bool,
    },
    /// Serve the web UI: inventory, coverage, start a run, watch it.
    Serve {
        source: String,
        /// Address to listen on (all interfaces by default).  There is no
        /// authentication: use 127.0.0.1:8080 on an untrusted network.
        #[arg(long, default_value = "0.0.0.0:8080")]
        listen: String,
        /// Where run-<time>.json and run-<time>.toml go.
        #[arg(long, default_value = "runs")]
        out_dir: PathBuf,
        /// The Finish action every run ends with.
        #[arg(long, value_enum, default_value = "halt")]
        finish: plan::Finish,
    },
}

fn with_port(source: &str) -> String {
    if source.contains(':') { source.to_string() } else { format!("{source}:{}", proto::c::SINK_LISTEN_PORT) }
}

fn human(n: u64) -> String {
    for (shift, unit) in [(40, "TiB"), (30, "GiB"), (20, "MiB"), (10, "KiB")] {
        if n >= 1 << shift {
            return format!("{:.1} {unit}", n as f64 / (1u64 << shift) as f64);
        }
    }
    format!("{n} B")
}

fn print_inventory(inv: &[proto::Entry]) {
    let mut nodes: Vec<&str> = inv.iter().map(|e| e.node.as_str()).collect();
    nodes.dedup();
    nodes.sort();
    nodes.dedup();
    for node in nodes {
        let mine: Vec<&proto::Entry> = inv.iter().filter(|e| e.node == node).collect();
        let sum = |k: Kind| mine.iter().filter(|e| e.kind == k).map(|e| e.size).sum::<u64>();
        println!(
            "{}: RAM {} labelable of {} declared, disks {}{}",
            if node.is_empty() { "(source)" } else { node },
            human(sum(Kind::Ram)),
            human(sum(Kind::Declared)),
            human(sum(Kind::Disk)),
            if sum(Kind::GpuDeclared) > 0 {
                format!(", GPU HBM {} labelable of {}", human(sum(Kind::Gpu)), human(sum(Kind::GpuDeclared)))
            } else {
                String::new()
            }
        );
        let mut map: Vec<&&proto::Entry> = mine.iter().collect();
        map.sort_by_key(|e| (e.base, e.kind != Kind::Declared));
        for e in map {
            match e.kind {
                Kind::Ram => println!("  ram       {:#014x}  {:>10}  target {}", e.base, human(e.size), plan::entry_label(e)),
                Kind::Disk => println!(
                    "  disk      {:<14}  {:>10}  target {} (sector {})",
                    e.name,
                    human(e.size),
                    plan::entry_label(e),
                    e.sector
                ),
                Kind::Gpu => println!("  gpu       {:<14}  {:>10}  target {}", e.name, human(e.size), plan::entry_label(e)),
                k => println!(
                    "  {:<9} {:#014x}  {:>10}  {}",
                    format!("{k:?}").to_lowercase(),
                    e.base,
                    human(e.size),
                    e.name
                ),
            }
        }
    }
}

fn main() -> Result<()> {
    match Cli::parse().cmd {
        Cmd::Serve { source, listen, out_dir, finish } => {
            web::serve(with_port(&source), listen, out_dir, plan::Params { finish, ..Default::default() })?;
        }
        Cmd::Inventory { source } => {
            let (_, inv) = engine::inventory(&with_port(&source))?;
            print_inventory(&inv);
        }
        Cmd::Plan { source, output, finish, exclude } => {
            let source = with_port(&source);
            let (_, inv) = engine::inventory(&source)?;
            let params = plan::Params { finish, ..Default::default() };
            let (p, skipped) = plan::default_plan(&inv, &params, |l| !exclude.iter().any(|x| x == l))?;
            let mut header = format!("Default plan for {source}");
            for s in &skipped {
                header.push_str(&format!("\nleft out: {}: {}", s.target, s.reason));
            }
            let text = plan::to_toml(&p, &header);
            match output {
                Some(f) => std::fs::write(f, text)?,
                None => print!("{text}"),
            }
        }
        Cmd::Run { source, plan: plan_file, finish, exclude, out_dir, threads, dry_run } => {
            let source = with_port(&source);
            let (conn, inv) = engine::inventory(&source)?;
            let mut p = match &plan_file {
                Some(f) => plan::from_toml(&std::fs::read_to_string(f)?)?,
                None => {
                    let (p, skipped) =
                        plan::default_plan(&inv, &plan::Params::default(), |l| !exclude.iter().any(|x| x == l))?;
                    for s in skipped {
                        println!("left out: {}: {}", s.target, s.reason);
                    }
                    p
                }
            };
            if let Some(f) = finish {
                p.finish = f;
            }
            for (i, s) in p.sessions.iter().enumerate() {
                println!(
                    "session {i}: {} {} chunk_blocks={} {} {} challenges, delta {} ms",
                    s.label(),
                    human(s.size),
                    s.chunk_blocks,
                    s.algo.name(),
                    s.challenges,
                    s.delta_ms
                );
            }
            let v = plan::validate(&p, &inv);
            for n in &v.notes {
                println!("  {n}");
            }
            if dry_run {
                for pr in &v.problems {
                    println!("PROBLEM: {pr}");
                }
                println!("dry run: nothing sent");
                return Ok(());
            }
            let (mut run, ok) = engine::Run::new(&source, &p, inv, &out_dir, Box::new(|m| println!("{m}")))?;
            let (json, toml) = run.record_paths();
            println!("record: {}  plan: {}", json.display(), toml.display());
            if !ok {
                for pr in &run.status.lock().unwrap().validation.problems {
                    println!("PROBLEM: {pr}");
                }
                bail!("plan rejected; nothing was sent");
            }
            if let Some(t) = threads {
                run.threads = t;
            }
            match run.execute(conn) {
                engine::Phase::Passed => {}
                ph => bail!("run ended {ph:?}"),
            }
        }
    }
    Ok(())
}
