//! The web front end: `pose-verifier serve SOURCE` serves one page on all
//! interfaces by default (no authentication: keep it off untrusted networks).
//!
//!   GET  /             the page; the start form is rendered here, so starting
//!                      a run is a plain HTML form post
//!   GET  /api/state    everything the page shows, as JSON, polled once a second
//!   POST /refresh      fetch the inventory again (clears a finished run)
//!   POST /run          start a run over the ticked targets (`t=<label>`...)
//!   GET  /files/run.json, /files/run.toml   the current run's record and plan
//!   GET  /app.js, /style.css
//!
//! The inventory is fetched at startup and on Refresh only, never polled: each
//! fetch is a connection the tray logs.  A run uses its own connection, opened
//! when Start is pressed, and runs on its own thread with the same engine as
//! the CLI; the page reads its RunStatus.

use crate::engine::{self, Phase, RunStatus};
use crate::plan::{self, Params};
use crate::proto::{Entry, Kind};
use anyhow::Result;
use axum::body::Bytes;
use axum::extract::{Path, State};
use axum::http::{header, StatusCode};
use axum::response::{Html, IntoResponse, Redirect, Response};
use axum::routing::{get, post};
use axum::Router;
use serde::Serialize;
use std::collections::{HashSet, VecDeque};
use std::fmt::Write as _;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

const INDEX: &str = include_str!("../web/index.html");
const APP_JS: &str = include_str!("../web/app.js");
const STYLE: &str = include_str!("../web/style.css");
const LOG_LINES: usize = 200;

struct RunHandle {
    status: Arc<Mutex<RunStatus>>,
    json: PathBuf,
    toml: PathBuf,
}

struct App {
    source: String,
    out_dir: PathBuf,
    params: Params,
    chunk_blocks: u32,
    inventory: Option<Vec<Entry>>,
    inventory_at: Option<u64>,
    inventory_error: Option<String>,
    run: Option<RunHandle>,
    /// Shown once on the next page load (a refused Start, a refused Refresh).
    message: Option<String>,
    /// Lines the engine logs, newest last.
    log: Arc<Mutex<VecDeque<String>>>,
}

type Shared = Arc<Mutex<App>>;

impl App {
    fn running(&self) -> bool {
        self.run.as_ref().is_some_and(|r| !r.status.lock().unwrap().phase.is_final())
    }
}

fn now() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Fetch the inventory (blocking) and store it, or the error.
fn refresh_inventory(app: &Shared) {
    let source = app.lock().unwrap().source.clone();
    let r = engine::inventory(&source);
    let mut a = app.lock().unwrap();
    a.inventory_at = Some(now());
    match r {
        Ok((_conn, inv)) => {
            a.inventory = Some(inv);
            a.inventory_error = None;
        }
        Err(e) => {
            a.inventory = None;
            a.inventory_error = Some(format!("{e:#}"));
        }
    }
}

pub fn serve(source: String, listen: String, out_dir: PathBuf, params: Params) -> Result<()> {
    let chunk_blocks = params.chunk_blocks;
    let app: Shared = Arc::new(Mutex::new(App {
        source,
        out_dir,
        params,
        chunk_blocks,
        inventory: None,
        inventory_at: None,
        inventory_error: None,
        run: None,
        message: None,
        log: Arc::new(Mutex::new(VecDeque::new())),
    }));
    refresh_inventory(&app);

    let router = Router::new()
        .route("/", get(index))
        .route("/api/state", get(state))
        .route("/refresh", post(refresh))
        .route("/run", post(start))
        .route("/files/{name}", get(file))
        .route("/app.js", get(|| async { ([(header::CONTENT_TYPE, "text/javascript")], APP_JS) }))
        .route("/style.css", get(|| async { ([(header::CONTENT_TYPE, "text/css")], STYLE) }))
        .with_state(app);

    let rt = tokio::runtime::Runtime::new()?;
    rt.block_on(async move {
        let l = tokio::net::TcpListener::bind(&listen).await?;
        let addr = l.local_addr()?;
        if !addr.ip().is_loopback() {
            eprintln!("note: listening on {addr} with no authentication; anyone who can reach it can start an erasure");
        }
        println!("serving on http://{addr}/");
        axum::serve(l, router).await?;
        Ok(())
    })
}

// ── state ─────────────────────────────────────────────────────────────────

#[derive(Serialize)]
struct ParamsView {
    algo: &'static str,
    delta_ms: u64,
    challenges: u32,
    chunk_blocks: u32,
    finish: &'static str,
}

#[derive(Serialize)]
struct StateView {
    source: String,
    now: u64,
    params: ParamsView,
    inventory: Option<Vec<Entry>>,
    inventory_at: Option<u64>,
    inventory_error: Option<String>,
    run: Option<RunStatus>,
    running: bool,
    log: Vec<String>,
}

async fn state(State(app): State<Shared>) -> Response {
    let a = app.lock().unwrap();
    let run = a.run.as_ref().map(|r| r.status.lock().unwrap().clone());
    let v = StateView {
        source: a.source.clone(),
        now: now(),
        params: ParamsView {
            algo: match &a.inventory {
                Some(inv) => plan::algo_for(inv).name(),
                None => "blake3 on a GPU, aes_prf on a CPU",
            },
            delta_ms: a.params.delta_ms,
            challenges: a.params.challenges,
            chunk_blocks: a.chunk_blocks,
            finish: a.params.finish.name(),
        },
        inventory: a.inventory.clone(),
        inventory_at: a.inventory_at,
        inventory_error: a.inventory_error.clone(),
        running: run.as_ref().is_some_and(|r| !r.phase.is_final()),
        run,
        log: a.log.lock().unwrap().iter().cloned().collect(),
    };
    ([(header::CACHE_CONTROL, "no-store")], axum::Json(v)).into_response()
}

// ── the page ──────────────────────────────────────────────────────────────

fn esc(s: &str) -> String {
    s.replace('&', "&amp;").replace('<', "&lt;").replace('>', "&gt;").replace('"', "&quot;")
}

fn human(n: u64) -> String {
    for (shift, unit) in [(40, "TiB"), (30, "GiB"), (20, "MiB"), (10, "KiB")] {
        if n >= 1 << shift {
            return format!("{:.1} {unit}", n as f64 / (1u64 << shift) as f64);
        }
    }
    format!("{n} B")
}

/// The start form: one ticked checkbox per target, in labelling order.
fn render_form(a: &App, inv: &[Entry]) -> String {
    let unit = a.chunk_blocks as u64 * 32;
    let mut h = String::new();
    h.push_str(r#"<form method="post" action="/run" id="start-form"><table class="targets"><thead><tr><th></th><th>Target</th><th>Kind</th><th class="num">Size</th><th class="num">Labelled</th><th class="num">Tail</th></tr></thead><tbody>"#);
    let (mut total_size, mut total_use) = (0u64, 0u64);
    for e in plan::ordered_targets(inv) {
        let label = plan::entry_label(e);
        let usable = plan::usable_size(e, a.chunk_blocks);
        let use_ = usable - usable % unit;
        total_size += e.size;
        total_use += use_;
        let (check, note) = if use_ == 0 {
            ("disabled", "smaller than one super-chunk".to_string())
        } else {
            ("checked", human(use_))
        };
        let _ = write!(
            h,
            r#"<tr><td><input type="checkbox" name="t" value="{v}" {check}></td><td class="mono">{l}</td><td>{k}</td><td class="num">{s}</td><td class="num">{note}</td><td class="num">{tail}</td></tr>"#,
            v = esc(&label),
            l = esc(&label),
            k = match e.kind {
                Kind::Ram => "RAM",
                Kind::Gpu => "GPU HBM",
                _ => "Disk",
            },
            s = human(e.size),
            tail = if e.size > use_ && use_ > 0 { human(e.size - use_) } else { String::new() },
        );
    }
    // Totals over every target, ticked or not (the tail is whatever a run
    // would leave: rounding, GPU scratch, and targets too small to label).
    let _ = write!(
        h,
        r#"</tbody><tfoot><tr><td></td><td>Total</td><td></td><td class="num">{s}</td><td class="num">{l}</td><td class="num">{t}</td></tr></tfoot>"#,
        s = human(total_size),
        l = human(total_use),
        t = if total_size > total_use { human(total_size - total_use) } else { String::new() },
    );
    let _ = write!(
        h,
        r#"</table><p class="warn">Starting erases every ticked target. The tray is sent <b>Finish: {f}</b> when the run ends, whatever the verdict.</p><button type="submit" class="primary">Start erasure</button></form>"#,
        f = a.params.finish.name()
    );
    h
}

async fn index(State(app): State<Shared>) -> Html<String> {
    let mut a = app.lock().unwrap();
    let message = a.message.take();
    let mut body = String::new();
    if let Some(m) = message {
        let _ = write!(body, r#"<div class="banner">{}</div>"#, esc(&m));
    }
    let running = a.running();
    if a.run.is_some() {
        body.push_str(r#"<section id="run"></section>"#);
    } else {
        match (&a.inventory, &a.inventory_error) {
            (Some(inv), _) => {
                body.push_str(r#"<section><h2>Targets</h2>"#);
                body.push_str(&render_form(&a, inv));
                body.push_str("</section>");
            }
            (None, Some(e)) => {
                let _ = write!(body, r#"<div class="banner">Cannot fetch the inventory: {}</div>"#, esc(e));
            }
            (None, None) => body.push_str("<p>No inventory yet.</p>"),
        }
    }
    body.push_str(r#"<section><h2>Coverage</h2><div id="legend"></div><div id="coverage"></div></section>"#);
    if a.run.is_some() {
        body.push_str(r#"<section><h2>Log</h2><pre id="log"></pre></section>"#);
    }
    let refresh = format!(
        r#"<form method="post" action="/refresh" class="inline"><button type="submit" id="refresh"{}>Refresh inventory</button></form>"#,
        if running { " disabled" } else { "" }
    );
    let html = INDEX
        .replace("{{SOURCE}}", &esc(&a.source))
        .replace("{{REFRESH}}", &refresh)
        .replace("{{BODY}}", &body);
    drop(a);
    Html(html)
}

// ── actions ───────────────────────────────────────────────────────────────

async fn refresh(State(app): State<Shared>) -> Redirect {
    {
        let mut a = app.lock().unwrap();
        if a.running() {
            a.message = Some("A run is in progress; the inventory cannot be refreshed until it ends.".into());
            return Redirect::to("/");
        }
        a.run = None;
        a.log.lock().unwrap().clear();
    }
    let app2 = app.clone();
    let _ = tokio::task::spawn_blocking(move || refresh_inventory(&app2)).await;
    Redirect::to("/")
}

async fn start(State(app): State<Shared>, body: Bytes) -> Redirect {
    let selected: HashSet<String> =
        form_urlencoded::parse(&body).filter(|(k, _)| k == "t").map(|(_, v)| v.into_owned()).collect();
    let app2 = app.clone();
    let r = tokio::task::spawn_blocking(move || start_blocking(&app2, selected)).await;
    let msg = match r {
        Ok(Ok(())) => None,
        Ok(Err(e)) => Some(format!("{e:#}")),
        Err(e) => Some(format!("internal error: {e}")),
    };
    if let Some(m) = msg {
        app.lock().unwrap().message = Some(m);
    }
    Redirect::to("/")
}

/// Open the run's connection, plan over the ticked targets of the inventory
/// it returns, validate, and start the run thread.
fn start_blocking(app: &Shared, selected: HashSet<String>) -> Result<()> {
    let (source, out_dir, params, log) = {
        let a = app.lock().unwrap();
        if a.running() {
            anyhow::bail!("a run is already in progress");
        }
        if a.run.is_some() {
            anyhow::bail!("the last run's result is still shown: refresh the inventory first");
        }
        (a.source.clone(), a.out_dir.clone(), a.params, a.log.clone())
    };
    let (conn, inv) = engine::inventory(&source)?;
    let (p, _skipped) = plan::default_plan(&inv, &params, |l| selected.contains(l))?;
    let v = plan::validate(&p, &inv);
    if !v.problems.is_empty() {
        // The connection is dropped unused: the tray holds nothing.
        anyhow::bail!("Not started: {}", v.problems.join("; "));
    }
    let logger = log.clone();
    let (run, _ok) = engine::Run::new(
        &source,
        &p,
        inv.clone(),
        &out_dir,
        Box::new(move |m| {
            println!("{m}");
            let mut l = logger.lock().unwrap();
            l.push_back(m.to_string());
            while l.len() > LOG_LINES {
                l.pop_front();
            }
        }),
    )?;
    let (json, toml) = run.record_paths();
    let handle = RunHandle { status: run.status.clone(), json: json.to_path_buf(), toml: toml.to_path_buf() };
    {
        let mut a = app.lock().unwrap();
        a.inventory = Some(inv);
        a.run = Some(handle);
    }
    std::thread::spawn(move || {
        let phase = run.execute(conn);
        if phase != Phase::Passed {
            eprintln!("run ended {phase:?}");
        }
    });
    Ok(())
}

async fn file(State(app): State<Shared>, Path(name): Path<String>) -> Response {
    let a = app.lock().unwrap();
    let Some(r) = &a.run else { return (StatusCode::NOT_FOUND, "no run").into_response() };
    let (path, ty) = match name.as_str() {
        "run.json" => (&r.json, "application/json"),
        "run.toml" => (&r.toml, "application/toml"),
        _ => return (StatusCode::NOT_FOUND, "no such file").into_response(),
    };
    let fname = path.file_name().map(|f| f.to_string_lossy().into_owned()).unwrap_or(name);
    match std::fs::read(path) {
        Ok(b) => (
            [(header::CONTENT_TYPE, ty.to_string()), (header::CONTENT_DISPOSITION, format!("attachment; filename=\"{fname}\""))],
            b,
        )
            .into_response(),
        Err(e) => (StatusCode::NOT_FOUND, e.to_string()).into_response(),
    }
}
