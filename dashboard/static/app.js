(() => {
'use strict';

const KEEP_SAMPLES = 6000;           // 10 min at 10 Hz
const CHARTS = [
  { key: 'temp_c',       title: 'Temperature', unit: '°C',  color: '--s1', digits: 2, minSpan: 4 },
  { key: 'power_w',      title: 'Power',       unit: 'W',   color: '--s2', digits: 2, minSpan: 1 },
  { key: 'fan_duty_pct', title: 'Fan duty',    unit: '%',   color: '--s3', digits: 0, fixed: [0, 100] },
  { key: 'fan_rpm',      title: 'Fan speed',   unit: 'RPM', color: '--s4', digits: 0, minSpan: 500, zero: true },
];
const PAD = { l: 48, r: 12, t: 8, b: 22 };
const X_STEP = { 30: 5, 60: 10, 300: 60 };

let samples = [];
let bmcId = null, boot = null;
let windowS = 60, paused = false, pausedEnd = 0;
let hoverT = null;
let gapS = 0.35;                     // break the line when samples are further apart than this
let serverUp = false;
let status = null;
let raf = 0;

const $ = (id) => document.getElementById(id);
const fmt = (v, d) => (v == null ? '—' : v.toFixed(d));

// ---- panels --------------------------------------------------------------
const chartsEl = $('charts');
for (const c of CHARTS) {
  const panel = document.createElement('div');
  panel.className = 'panel';
  panel.innerHTML =
    `<div class="panel-head">
       <div class="panel-title"><span class="swatch" style="background:var(${c.color})"></span>${c.title}</div>
       <div class="panel-val"><span class="num">—</span><small>${c.unit}</small></div>
     </div>
     <div class="panel-meta">&nbsp;</div>
     <canvas role="img" aria-label="${c.title} over time"></canvas>`;
  chartsEl.appendChild(panel);
  c.canvas = panel.querySelector('canvas');
  c.num = panel.querySelector('.num');
  c.meta = panel.querySelector('.panel-meta');
  c.canvas.addEventListener('pointermove', (e) => {
    const r = c.canvas.getBoundingClientRect();
    const frac = Math.min(1, Math.max(0, (e.clientX - r.left - PAD.l) / (r.width - PAD.l - PAD.r)));
    hoverT = viewEnd() - windowS + frac * windowS;
    schedule();
  });
  c.canvas.addEventListener('pointerleave', () => { hoverT = null; schedule(); });
}

// ---- data ----------------------------------------------------------------
function ingest(rec) {
  if (bmcId === null) bmcId = rec.bmc_id;
  if (rec.bmc_id !== bmcId) return;                  // v1 shows one BMC; ids are carried for later
  if (boot !== null && rec.boot !== boot) samples = []; // BMC rebooted: new time base
  boot = rec.boot;
  rec.t = rec.t_us / 1e6;
  const last = samples[samples.length - 1];
  if (last && rec.t <= last.t) return;
  samples.push(rec);
  if (samples.length > KEEP_SAMPLES + 1000) samples.splice(0, samples.length - KEEP_SAMPLES);
  if (samples.length >= 11) {
    const dts = [];
    for (let i = samples.length - 10; i < samples.length; i++) dts.push(samples[i].t - samples[i - 1].t);
    dts.sort((a, b) => a - b);
    gapS = Math.max(0.05, dts[5] * 3.5);
  }
}

const viewEnd = () => (paused ? pausedEnd : samples.length ? samples[samples.length - 1].t : 0);

function lowerBound(t) {                             // first index with samples[i].t >= t
  let lo = 0, hi = samples.length;
  while (lo < hi) { const m = (lo + hi) >> 1; if (samples[m].t < t) lo = m + 1; else hi = m; }
  return lo;
}
function nearest(t) {
  const i = lowerBound(t);
  if (i === 0) return samples[0];
  if (i >= samples.length) return samples[samples.length - 1];
  return t - samples[i - 1].t < samples[i].t - t ? samples[i - 1] : samples[i];
}

// ---- drawing -------------------------------------------------------------
function niceTicks(lo, hi, n) {
  const raw = (hi - lo) / n, p = 10 ** Math.floor(Math.log10(raw)), m = raw / p;
  const step = (m <= 1 ? 1 : m <= 2 ? 2 : m <= 5 ? 5 : 10) * p, out = [];
  for (let v = Math.ceil(lo / step) * step; v <= hi + 1e-9; v += step) out.push(+v.toFixed(10));
  return out;
}

function yRange(c, vis) {
  if (c.fixed) return c.fixed;
  let lo = Infinity, hi = -Infinity;
  for (const s of vis) { const v = s[c.key]; if (v != null) { if (v < lo) lo = v; if (v > hi) hi = v; } }
  if (lo === Infinity) return [0, c.minSpan || 1];
  if (c.zero) lo = Math.min(lo, 0);
  if (hi - lo < c.minSpan) {
    const mid = (hi + lo) / 2;
    lo = c.zero ? 0 : mid - c.minSpan / 2; hi = c.zero ? c.minSpan : mid + c.minSpan / 2;
  } else {
    const pad = (hi - lo) * 0.1; hi += pad; if (!c.zero) lo -= pad;
  }
  return [lo, hi];
}

function drawChart(c, css, tEnd) {
  const cv = c.canvas, dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = cv.clientHeight;
  if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
    cv.width = Math.round(w * dpr); cv.height = Math.round(h * dpr);
  }
  const ctx = cv.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  const pw = w - PAD.l - PAD.r, ph = h - PAD.t - PAD.b, t0 = tEnd - windowS;
  const col = (name) => css.getPropertyValue(name).trim();
  const vis = samples.slice(lowerBound(t0));
  const [lo, hi] = yRange(c, vis);
  const X = (t) => PAD.l + ((t - t0) / windowS) * pw;
  const Y = (v) => PAD.t + (1 - (v - lo) / (hi - lo)) * ph;

  ctx.font = '11px system-ui, sans-serif';
  ctx.lineWidth = 1;
  ctx.textAlign = 'right'; ctx.textBaseline = 'middle';
  for (const v of niceTicks(lo, hi, 4)) {           // recessive grid + y labels
    const y = Math.round(Y(v)) + 0.5;
    ctx.strokeStyle = col('--grid'); ctx.beginPath(); ctx.moveTo(PAD.l, y); ctx.lineTo(w - PAD.r, y); ctx.stroke();
    ctx.fillStyle = col('--text-3'); ctx.fillText(String(+v.toFixed(2)), PAD.l - 6, y);
  }
  ctx.textAlign = 'center'; ctx.textBaseline = 'top';
  const step = X_STEP[windowS] || 10;
  for (let k = 0; k * step <= windowS; k++) {
    const x = X(tEnd - k * step);
    ctx.fillStyle = col('--text-3');
    ctx.fillText(k === 0 ? 'now' : `−${k * step}s`, Math.min(Math.max(x, PAD.l + 8), w - PAD.r - 8), h - PAD.b + 6);
  }

  ctx.save();                                         // series line, broken at gaps and missing values
  ctx.beginPath(); ctx.rect(PAD.l, PAD.t - 2, pw, ph + 4); ctx.clip();
  ctx.strokeStyle = col(c.color); ctx.lineWidth = 2; ctx.lineJoin = 'round'; ctx.lineCap = 'round';
  ctx.beginPath();
  let prev = null;
  for (const s of vis) {
    const v = s[c.key];
    if (v == null) { prev = null; continue; }
    if (prev && s.t - prev.t <= gapS) ctx.lineTo(X(s.t), Y(v)); else ctx.moveTo(X(s.t), Y(v));
    prev = s;
  }
  ctx.stroke();
  ctx.restore();

  const dot = (s) => {                                // marker with a surface ring
    if (!s || s[c.key] == null || s.t < t0 || s.t > tEnd) return;
    ctx.beginPath(); ctx.arc(X(s.t), Y(s[c.key]), 4, 0, Math.PI * 2);
    ctx.fillStyle = col(c.color); ctx.fill();
    ctx.lineWidth = 2; ctx.strokeStyle = col('--surface'); ctx.stroke();
  };
  const latest = samples[samples.length - 1];
  let shown = latest;
  if (hoverT != null && samples.length) {
    shown = nearest(hoverT);
    const x = Math.round(X(shown.t)) + 0.5;
    ctx.strokeStyle = col('--text-3'); ctx.lineWidth = 1;
    ctx.beginPath(); ctx.moveTo(x, PAD.t); ctx.lineTo(x, PAD.t + ph); ctx.stroke();
    dot(shown);
  } else {
    dot(vis[vis.length - 1]);
  }

  c.num.textContent = fmt(shown && shown[c.key], c.digits);
  if (!shown) c.meta.textContent = ' ';
  else if (hoverT != null) c.meta.textContent = `${(shown.t - latest.t).toFixed(1)} s · seq ${shown.seq}`;
  else if (c.key === 'fan_duty_pct') c.meta.textContent = shown.fan_en === 0 ? 'Fan DISABLED (interlock)' : 'Fan enabled';
  else c.meta.textContent = shown[c.key] == null ? 'no valid reading' : ' ';
}

function render() {
  raf = 0;
  const css = getComputedStyle(document.documentElement);
  const tEnd = viewEnd();
  for (const c of CHARTS) drawChart(c, css, tEnd);
  if ($('tbl').open) {
    $('tbody').innerHTML = samples.slice(-15).reverse().map((s) =>
      `<tr><td>${s.t.toFixed(1)}</td><td>${s.seq}</td><td>${fmt(s.temp_c, 3)}</td><td>${fmt(s.power_w, 3)}</td>` +
      `<td>${fmt(s.fan_duty_pct, 1)}</td><td>${s.fan_en === 0 ? 'off' : 'on'}</td><td>${fmt(s.fan_rpm, 0)}</td></tr>`).join('');
  }
}
function schedule() { if (!raf) raf = requestAnimationFrame(render); }

// ---- status / link badge ---------------------------------------------------
function setLink(state, text) {
  $('link').dataset.state = state;
  $('linkText').textContent = text;
}
function applyStatus(s) {
  status = s;
  if (s.age_s == null) setLink('waiting', 'Waiting for BMC…');
  else if (s.age_s <= 1.5) setLink('live', 'Live');
  else setLink('stale', `No data for ${Math.round(s.age_s)} s`);
  $('sub').textContent = bmcId
    ? `BMC ${bmcId} · ${s.bmc_addr} · ${s.rate_hz} Hz${s.bmc_ids.length > 1 ? ` · ${s.bmc_ids.length} BMCs seen, showing ${bmcId}` : ''}`
    : `Looking for BMC at ${s.bmc_addr}…`;
  $('stRate').textContent = `${s.rate_hz} Hz`;
  $('stRecv').textContent = s.received.toLocaleString();
  $('stLost').textContent = s.lost.toLocaleString();
  $('stLost').classList.toggle('bad', s.lost > 0);
  $('stDrops').textContent = s.bmc_drops == null ? '—' : s.bmc_drops.toLocaleString();
  $('stDrops').classList.toggle('bad', s.bmc_drops > 0);
  $('stRestarts').textContent = s.restarts;
  $('stLog').textContent = s.log_file || 'off';
}

// ---- controls ------------------------------------------------------------
document.querySelectorAll('[data-win]').forEach((b) => b.addEventListener('click', () => {
  windowS = +b.dataset.win;
  document.querySelectorAll('[data-win]').forEach((o) => o.setAttribute('aria-pressed', String(o === b)));
  schedule();
}));
$('pause').addEventListener('click', () => {
  paused = !paused;
  pausedEnd = viewEnd() || (samples.length ? samples[samples.length - 1].t : 0);
  if (paused && samples.length) pausedEnd = samples[samples.length - 1].t;
  $('pause').setAttribute('aria-pressed', String(paused));
  $('pause').textContent = paused ? 'Resume' : 'Pause';
  schedule();
});
const THEMES = ['auto', 'light', 'dark'];
function applyTheme(mode) {
  if (mode === 'auto') delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = mode;
  $('theme').textContent = `Theme: ${mode}`;
  $('theme').dataset.mode = mode;
  schedule();
}
$('theme').addEventListener('click', () => {
  const next = THEMES[(THEMES.indexOf($('theme').dataset.mode) + 1) % THEMES.length];
  applyTheme(next);
  try { localStorage.setItem('bmc-theme', next); } catch (e) { /* storage unavailable */ }
});
let savedTheme = 'auto';
try { savedTheme = localStorage.getItem('bmc-theme') || 'auto'; } catch (e) { /* storage unavailable */ }
applyTheme(THEMES.includes(savedTheme) ? savedTheme : 'auto');
window.addEventListener('resize', schedule);
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', schedule);
$('tbl').addEventListener('toggle', schedule);

// ---- live connection -----------------------------------------------------
function connect() {
  const es = new EventSource('events');
  es.addEventListener('open', () => { serverUp = true; });
  es.addEventListener('error', () => { serverUp = false; setLink('offline', 'Dashboard server unreachable'); });
  es.addEventListener('sample', (e) => { ingest(JSON.parse(e.data)); schedule(); });
  es.addEventListener('status', (e) => { serverUp = true; applyStatus(JSON.parse(e.data)); });
}
fetch('api/history?n=' + KEEP_SAMPLES)
  .then((r) => r.json())
  .then((h) => { h.samples.forEach(ingest); schedule(); })
  .catch(() => { /* server will be retried by EventSource */ })
  .finally(connect);
schedule();
})();
