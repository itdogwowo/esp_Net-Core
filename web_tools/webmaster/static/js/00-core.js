/* ESP WebMaster — 核心：狀態、格式化、toast、fetch 包裝
 * 所有模組共用的最小底層；呢個一定係第一個載入。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

/* ESP WebMaster — mp_Tools 風格外殼；原生 JS，零 build step。
 * 只經 REST/WS 講嘢；NC4 由 Python 側處理。
 */
"use strict";

const $ = (s) => document.querySelector(s);
const $$ = (s) => Array.from(document.querySelectorAll(s));

const state = {
  connected: false,
  port: null,
  sim: false,
  view: "device",
  cwd: "/",
  file: null,
  dirty: false,
  ws: null,
  logPaused: false,
  logs: [],
  fwFile: null,
  espFile: null,
  commands: [],
  logDebug: false,
  manifest: {},
  driveRoot: "",
  drivePath: "/",
  driveFile: null,
  driveDirty: false,
  // IDE
  ideTabs: [],
  ideActive: null,
  ideCM: null,
  ideSilent: false,
  lastSeq: 0,
  session: "",
  device: "",
  switching: false,
};

// ── 格式化 ─────────────────────────────────────────────
function fmtBytes(n) {
  if (n === null || n === undefined || isNaN(n)) return "—";
  n = Number(n);
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${(n / 1024 / 1024).toFixed(2)} MiB`;
}

function fmtUptime(s) {
  if (s === null || s === undefined) return "—";
  s = Number(s);
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60), sec = s % 60;
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  if (m) return `${m}m ${sec}s`;
  return `${sec}s`;
}

function joinPath(dir, name) {
  if (!name) return dir;
  if (name.startsWith("/")) return name;
  return dir === "/" ? "/" + name : dir.replace(/\/+$/, "") + "/" + name;
}

function baseName(p) {
  return (p || "").replace(/\/+$/, "").split("/").pop() || "/";
}

function hexToBytes(hex) {
  const clean = (hex || "").replace(/[\s:]/g, "");
  if (!clean) return new Uint8Array();
  if (clean.length % 2 || /[^0-9a-fA-F]/.test(clean)) throw new Error("hex 格式錯");
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16);
  return out;
}

function decodeText(buf) {
  if (!buf || !buf.length) return "";
  if (buf.subarray(0, 512).includes(0)) return null;
  try { return new TextDecoder("utf-8", { fatal: true }).decode(buf); }
  catch (_) { return new TextDecoder("utf-8").decode(buf); }
}

// ── toast ──────────────────────────────────────────────
function toast(msg, kind = "ok", ms = 3400) {
  const el = document.createElement("div");
  el.className = "toast";
  el.dataset.kind = kind;
  const ic = document.createElement("span");
  ic.className = "ic";
  ic.textContent = kind === "err" ? "✕" : kind === "warn" ? "!" : "✓";
  const wrap = document.createElement("div");
  const tt = document.createElement("div");
  tt.className = "tt";
  tt.textContent = kind === "err" ? "失敗" : kind === "warn" ? "注意" : "完成";
  const tb = document.createElement("div");
  tb.className = "tb";
  tb.textContent = String(msg ?? "");
  wrap.append(tt, tb);
  el.append(ic, wrap);
  $("#toasts").appendChild(el);
  setTimeout(() => el.remove(), ms);
}

async function api(url, opts = {}) {
  const headers = { "X-Actor": "ui", ...(opts.headers || {}) };
  const r = await fetch(url, { ...opts, headers });
  const ct = r.headers.get("content-type") || "";
  if (!r.ok) {
    let msg = `${r.status} ${r.statusText}`;
    try {
      const j = await r.json();
      msg = typeof j.detail === "string" ? j.detail : JSON.stringify(j.detail ?? j);
    } catch (_) { /* 非 JSON */ }
    if (r.status === 409) setConnected(false);
    throw new Error(msg);
  }
  return ct.includes("application/json") ? r.json() : r;
}
