// Polls /api/state once a second and draws the coverage bars and the run.
// Starting a run and refreshing the inventory are plain form posts rendered
// by the server; nothing here changes the tray.
"use strict";

const COLOURS = {
  kernel: "--c-kernel", reserved: "--c-reserved", unusable: "--c-unusable",
  free: "--c-free", tail: "--c-tail", planned: "--c-planned", generating: "--c-generating",
  stored: "--c-stored", passed: "--c-passed", failed: "--c-failed",
};
const LEGEND = [
  ["kernel", "kernel (System RAM)"], ["reserved", "firmware reserved"], ["unusable", "too small / not mappable"],
  ["free", "labellable, not in plan"], ["tail", "labellable, not covered (tail)"], ["planned", "planned"],
  ["generating", "generating"], ["stored", "stored"], ["passed", "proven"], ["failed", "failed"],
];
const PHASE = {
  labelling: "Labelling", expecting: "Computing expected labels", challenging: "Challenging",
  passed: "Proof accepted", rejected: "Proof rejected", failed: "Failed",
};

const $ = (id) => document.getElementById(id);
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

function human(n) {
  for (const [shift, unit] of [[40, "TiB"], [30, "GiB"], [20, "MiB"], [10, "KiB"]]) {
    if (n >= 2 ** shift) return (n / 2 ** shift).toFixed(1) + " " + unit;
  }
  return n + " B";
}
function hex(n) { return "0x" + n.toString(16); }
function duration(s) {
  s = Math.max(0, Math.round(s));
  if (s < 60) return s + "s";
  if (s < 3600) return Math.floor(s / 60) + "m" + String(s % 60).padStart(2, "0") + "s";
  return Math.floor(s / 3600) + "h" + String(Math.floor(s / 60) % 60).padStart(2, "0") + "m";
}
// Same as plan::target_label in Rust: what the form's checkboxes carry.
function targetLabel(node, name, base, isRam) {
  const n = isRam ? `${name}@${hex(base)}` : name;
  return node ? `${node}:${n}` : n;
}
function depth(node) { return node ? node.split("/").length : 0; }

// ── coverage model ──────────────────────────────────────────────────────────

// Segments for one address space: [{start, end, cls, title, ticks?}] (end exclusive).
// The plan comes from the checkboxes before a run, from the run's sessions after.
function segmentsFor(entries, targets, sessions, unit, checked) {
  const segs = [];
  for (const e of entries) {
    if (["kernel", "reserved", "unusable"].includes(e.kind)) {
      segs.push({ start: e.base, end: e.base + e.size, cls: e.kind, title: `${e.name}: ${hex(e.base)} +${human(e.size)}` });
    }
  }
  for (const t of targets) {
    const label = targetLabel(t.node, t.name, t.base, t.kind === "ram");
    const start = t.kind === "ram" ? t.base : 0;
    const end = start + t.size;
    const mine = sessions ? sessions.filter((s) => s.plan.node === t.node && s.plan.target === t.name &&
      s.plan.base >= t.base && s.plan.base < t.base + t.size) : null;
    if (sessions && mine.length === 0) {
      segs.push({ start, end, cls: "free", title: `${label}: not in plan, ${human(t.size)}` });
      continue;
    }
    if (!sessions) {
      const use = t.size - (t.size % unit);
      if (!checked.has(label) || use === 0) {
        segs.push({ start, end, cls: "free", title: `${label}: not in plan, ${human(t.size)}` });
        continue;
      }
      segs.push({ start, end: start + use, cls: "planned", title: `${label}: planned, ${human(use)}` });
      if (use < t.size) segs.push({ start: start + use, end, cls: "tail", title: `${label}: tail not covered, ${human(t.size - use)}` });
      continue;
    }
    let covered = start;
    for (const s of mine) {
      const sb = s.plan.base, se = sb + s.plan.size;
      const title = `session ${s.number}: ${label}, ${human(s.plan.size)}, ${s.state}` + (s.verdict ? ` — ${s.verdict}` : "");
      if (s.state === "generating" && s.bytes_done > 0) {
        segs.push({ start: sb, end: sb + s.bytes_done, cls: "generating", title });
        segs.push({ start: sb + s.bytes_done, end: se, cls: "planned", title });
      } else {
        const cls = { planned: "planned", generating: "planned", stored: "stored", passed: "passed", failed: "failed" }[s.state];
        const ticks = (s.failed_blocks || []).map((b) => sb + b * 32);
        segs.push({ start: sb, end: se, cls, title, ticks });
      }
      covered = Math.max(covered, se);
    }
    if (covered < end) segs.push({ start: covered, end, cls: "tail", title: `${label}: tail not covered, ${human(end - covered)}` });
  }
  return segs;
}

// Draw segments on a bar whose x axis is the concatenation of `ranges`
// ([start, end) pairs), so holes between declared DRAM ranges take no space.
function bar(ranges, segs) {
  const total = ranges.reduce((a, [s, e]) => a + (e - s), 0) || 1;
  const W = 1000;
  const x = (addr) => {
    let off = 0;
    for (const [s, e] of ranges) {
      if (addr <= e) return ((off + Math.max(0, addr - s)) / total) * W;
      off += e - s;
    }
    return W;
  };
  let svg = `<svg class="bar" viewBox="0 0 ${W} 20" preserveAspectRatio="none" role="img">`;
  for (const g of segs) {
    const x0 = x(g.start), w = Math.max(x(g.end) - x0, 1.2);
    svg += `<rect x="${x0.toFixed(2)}" y="0" width="${w.toFixed(2)}" height="20" fill="var(${COLOURS[g.cls]})"><title>${esc(g.title)}</title></rect>`;
    for (const t of g.ticks || []) {
      svg += `<rect x="${x(t).toFixed(2)}" y="0" width="1.5" height="20" fill="var(--c-failed)"><title>${esc("wrong label at " + hex(t))}</title></rect>`;
    }
  }
  return svg + "</svg>";
}

function drawCoverage(st) {
  const inv = st.inventory || [];
  const run = st.run;
  const unit = st.params.chunk_blocks * 32;
  const checked = new Set([...document.querySelectorAll('#start-form input[name="t"]:checked')].map((c) => c.value));
  const sessions = run ? run.sessions : null;
  const nodes = [...new Set(inv.map((e) => e.node))].sort((a, b) => depth(b) - depth(a) || a.localeCompare(b));
  let html = "";
  for (const node of nodes) {
    const mine = inv.filter((e) => e.node === node);
    const declared = mine.filter((e) => e.kind === "declared").sort((a, b) => a.base - b.base);
    const ram = mine.filter((e) => e.kind !== "disk");
    const disks = mine.filter((e) => e.kind === "disk");
    const sum = (k) => mine.filter((e) => e.kind === k).reduce((a, e) => a + e.size, 0);
    html += `<div class="node"><div class="node-head"><b>${esc(node || "tray host (source)")}</b>` +
      `<span>RAM ${human(sum("ram"))} labellable of ${human(sum("declared"))} declared · kernel ${human(sum("kernel"))} · reserved ${human(sum("reserved"))} · unusable ${human(sum("unusable"))}</span></div>`;
    if (declared.length) {
      const ranges = declared.map((d) => [d.base, d.base + d.size]);
      const segs = segmentsFor(ram, ram.filter((e) => e.kind === "ram"), sessions, unit, checked);
      html += `<div class="bar-row"><div class="bar-label">DRAM ${hex(declared[0].base)}…</div>${bar(ranges, segs)}</div>`;
    }
    for (const d of disks) {
      const segs = segmentsFor([], [d], sessions, unit, checked);
      html += `<div class="bar-row"><div class="bar-label mono" title="${esc(d.name)}">${esc(d.name)} · ${human(d.size)}</div>${bar([[0, d.size]], segs)}</div>`;
    }
    html += "</div>";
  }
  if (!inv.length) html = `<p class="sub">${st.inventory_error ? "No inventory: " + esc(st.inventory_error) : "No inventory."}</p>`;
  $("coverage").innerHTML = html;
}

// ── run view ────────────────────────────────────────────────────────────────

function drawRun(st) {
  const r = st.run;
  const el = $("run");
  if (!el || !r) return;
  // The tray's whole capacity: every node's declared DRAM plus its disks.
  const declared = (st.inventory || []).filter((e) => e.kind === "declared" || e.kind === "disk").reduce((a, e) => a + e.size, 0);
  const sumState = (f) => r.sessions.filter(f).reduce((a, s) => a + s.plan.size, 0);
  const proven = sumState((s) => s.state === "passed");
  const stored = sumState((s) => ["stored", "passed", "failed"].includes(s.state));
  const planned = sumState(() => true);
  const elapsed = (r.ended || st.now) - r.started;
  const cur = r.sessions.find((s) => s.state === "generating");
  let html = `<div class="summary"><span class="phase ${r.phase}">${PHASE[r.phase] || r.phase}</span>` +
    `<span>elapsed <b>${duration(elapsed)}</b></span>` +
    `<span>proven <span class="big">${human(proven)}</span> of ${human(declared)} in the tray (DRAM + disks)</span>` +
    `<span>stored ${human(stored)} of ${human(planned)} planned</span>`;
  if (cur && cur.eta_s != null) html += `<span>session ${cur.number} ETA <b>${duration(cur.eta_s)}</b> at ${human(cur.rate)}/s</span>`;
  html += `<span><a href="/files/run.json">record (JSON)</a> · <a href="/files/run.toml">plan (TOML)</a></span></div>`;
  if (r.reason) html += `<p class="reason">${esc(r.reason)}${r.finish_sent ? "<br>Finish sent: " + esc(r.finish_sent) : ""}</p>`;
  html += `<table><thead><tr><th>#</th><th>Target</th><th class="num">Size</th><th>State</th><th>Progress</th>` +
    `<th class="num">Rate</th><th class="num">Expected labels</th><th class="num">Challenges</th><th class="num">RTT avg / max</th><th>Verdict</th></tr></thead><tbody>`;
  for (const s of r.sessions) {
    const label = targetLabel(s.plan.node, s.plan.target, s.plan.base, s.plan.target === "ram");
    const pct = s.plan.size ? (100 * s.bytes_done) / s.plan.size : 0;
    const prog = s.state === "planned" ? "" :
      `<span class="progress"><div style="width:${pct.toFixed(1)}%"></div></span>${pct.toFixed(0)}%` +
      (s.state === "generating" && s.eta_s != null ? ` · ${duration(s.eta_s)}` : "");
    const rate = s.generate_s ? human(s.plan.size / s.generate_s) + "/s" : s.rate ? human(s.rate) + "/s" : "";
    const exp = s.expected_chunks ? `${s.expected_done}/${s.expected_chunks} chunks` : "";
    const n = s.challenges_ok + s.challenges_bad;
    const ch = n ? `${s.challenges_ok}/${s.plan.challenges}` + (s.challenges_bad ? ` (${s.challenges_bad} bad)` : "") : "";
    const rtt = s.rtt ? `${s.rtt.avg_ms.toFixed(2)} / ${s.rtt.max_ms.toFixed(2)} ms` : "";
    html += `<tr><td>${s.number}</td><td class="mono">${esc(label)}</td><td class="num">${human(s.plan.size)}</td>` +
      `<td class="state ${s.state}">${s.state}</td><td>${prog}</td><td class="num">${rate}</td><td class="num">${exp}</td>` +
      `<td class="num">${ch}</td><td class="num">${rtt}</td><td>${esc(s.verdict || "")}</td></tr>`;
  }
  html += "</tbody></table>";
  if (r.validation && r.validation.notes.length) {
    html += `<h3>Parameters</h3><p class="sub">${esc(r.validation.notes[0].replace(/^session \d+ \([^)]*\): /, ""))}</p>`;
  }
  el.innerHTML = html;
  $("log").textContent = st.log.join("\n");
}

// ── loop ────────────────────────────────────────────────────────────────────

let last = null;
async function tick() {
  try {
    const res = await fetch("/api/state", { cache: "no-store" });
    const st = await res.json();
    // The server renders the start form or the run view; reload when that changes.
    if (!!st.run !== !!$("run")) { location.reload(); return; }
    const p = st.params;
    $("params").textContent = `${p.algo}, chunk_blocks ${p.chunk_blocks}, ${p.challenges} challenges/session, delta ${p.delta_ms} ms, Finish: ${p.finish}`;
    const rb = $("refresh");
    if (rb) rb.disabled = st.running;
    last = st;
    drawRun(st);
    drawCoverage(st);
  } catch (e) {
    $("params").textContent = "verifier not reachable: " + e;
  }
}

$("legend").innerHTML = LEGEND.map(([k, t]) => `<span><i style="background:var(${COLOURS[k]})"></i>${t}</span>`).join("");
document.addEventListener("change", (ev) => {
  if (ev.target.matches('#start-form input[name="t"]') && last) drawCoverage(last);
});
tick();
setInterval(tick, 1000);
