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
  commands: [],
  logDebug: false,
  manifest: {},
  driveRoot: "",
  drivePath: "/",
  driveFile: null,
  driveDirty: false,
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
  const r = await fetch(url, opts);
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

// ── 連線 ───────────────────────────────────────────────
function setConnected(ok, port = null, sim = false) {
  state.connected = ok;
  state.port = port;
  state.sim = sim;
  $("#led").dataset.state = ok ? "idle" : "error";
  $("#ticket").dataset.active = ok ? "true" : "false";
  $("#ticketName").textContent = ok ? (sim ? "模擬裝置" : "ESP") : "未連線";
  $("#ticketMeta").textContent = ok ? `${port || "—"}${sim ? " · sim" : ""}` : "—";
  $("#btnDisconnect").disabled = !ok;
  $("#btnConnect").disabled = ok;
  $("#btnSim").disabled = ok;
  $("#stOnline").textContent = ok ? "● online" : "● offline";
  $("#stOnline").className = ok ? "on" : "off";
  $("#dlPort").textContent = port || "—";
  if (!ok) {
    for (const id of ["dlChip", "dlVer", "dlUp", "dlHeap", "dlFree", "dlTotal",
      "dlPending", "dlProduced", "dlSent", "dlDropped", "stChip", "stVer",
      "stUptime", "stHeap", "stModeText", "stFree", "stTotal", "stPending",
      "stLogLvl", "stLogProd", "stLogSent", "stLogDrop", "stPort", "stMode2",
      "stFree2"]) {
      const el = document.getElementById(id);
      if (el) el.textContent = "—";
    }
    $("#sideMode").textContent = "—";
    $("#stMode").textContent = "—";
    $("#usedMeter").style.width = "0%";
    $("#statusRaw").textContent = "—";
    if ($("#ticketName")) $("#ticketName").textContent = "未連線";
  }
}

async function refreshPorts() {
  try {
    const j = await api("/api/ports");
    const sel = $("#portSelect");
    const ports = j.data || [];
    const keep = sel.value;
    sel.innerHTML = "";
    if (!ports.length) {
      const o = document.createElement("option");
      o.value = "";
      o.textContent = "（未偵測到序列埠）";
      sel.appendChild(o);
    }
    for (const p of ports) {
      const o = document.createElement("option");
      o.value = p.device;
      const desc = (p.description || "").replace(/\s*\(COM\d+\)\s*$/i, "");
      o.textContent = `${p.device} — ${desc || "序列裝置"}`;
      sel.appendChild(o);
    }
    if (keep) sel.value = keep;
  } catch (e) {
    console.warn("ports:", e);
  }
}

async function connect(port) {
  $("#led").dataset.state = "connecting";
  try {
    const j = await api("/api/connect", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(port === "sim" ? { sim: true } : { port }),
    });
    setConnected(true, j.port, j.sim);
    logLine(`連線 ${j.port}${j.sim ? "（模擬）" : ""}`, "ok");
    openWS();
    await refreshStatus();
    if (!(await loadManifest({ quiet: true }))) {
      logLine("未有 manifest：自動重建索引…", "warn");
      await rescan();
    }
    switchView("files");
  } catch (e) {
    setConnected(false);
    toast(`連線失敗：${e.message}`, "err", 6000);
    logLine(`連線失敗：${e.message}`, "err");
  }
}

async function disconnect() {
  try { await api("/api/disconnect", { method: "POST" }); } catch (_) { /* ignore */ }
  closeWS();
  setConnected(false);
  logLine("已斷線", "warn");
}

// ── WebSocket ──────────────────────────────────────────
function openWS() {
  closeWS();
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/ws/log`);
  state.ws = ws;
  ws.onmessage = (ev) => {
    try { handleEvent(JSON.parse(ev.data)); } catch (_) { /* ignore */ }
  };
  ws.onclose = () => {
    if (state.ws === ws) state.ws = null;
    if (state.connected) setTimeout(() => { if (state.connected && !state.ws) openWS(); }, 1500);
  };
}

function closeWS() {
  const ws = state.ws;
  state.ws = null;
  if (ws) { try { ws.close(); } catch (_) { /* ignore */ } }
}

function handleEvent(ev) {
  switch (ev.type) {
    case "hello":
      if (ev.connected && !state.connected) setConnected(true, ev.port, ev.sim);
      break;
    case "line":
      logLine(ev.text);
      break;
    case "frame":
      logLine(`«frame ${ev.cmd}  ${(ev.hex || "").length / 2} B»`, "frame");
      break;
    case "progress":
      if (ev.op === "upload") {
        setProgress("#fileProgress", "#fileProgressWrap", ev.done, ev.total);
        if (ev.total > 0 && ev.done >= ev.total) {
          setTimeout(() => $("#fileProgressWrap").classList.add("hidden"), 800);
        }
      }
      if (ev.op === "ota") {
        setProgress("#otaProgress", "#otaProgressWrap", ev.done, ev.total);
        if (ev.total > 0 && ev.done >= ev.total) {
          setTimeout(() => $("#otaProgressWrap").classList.add("hidden"), 800);
        }
      }
      break;
    case "disconnected":
      setConnected(false);
      logLine(`連線中斷：${ev.reason || "unknown"}`, "err");
      break;
    default:
      break;
  }
}

// ── 終端 ───────────────────────────────────────────────
function logLine(text, cls = "") {
  const line = String(text ?? "");
  state.logs.push(line);
  if (state.logs.length > 4000) state.logs.splice(0, state.logs.length - 4000);
  if (state.logPaused) return;
  const box = $("#terminal");
  const div = document.createElement("div");
  let k = cls;
  if (!k) {
    if (/^E\s/.test(line)) k = "err";
    else if (/^W\s/.test(line)) k = "warn";
    else if (/^I\s/.test(line)) k = "info";
    else if (/^D\s/.test(line)) k = "dim";
  }
  div.className = `line ${k}`;
  div.textContent = line;
  box.appendChild(div);
  while (box.childElementCount > 2500) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}

// ── 狀態 ───────────────────────────────────────────────
async function refreshStatus() {
  if (!state.connected) return;
  try {
    const j = await api("/api/status?keys=sys,storage,log");
    const d = j.data || {};
    const sys = d.sys || {};
    const st = d.storage || {};
    const lg = d.log || {};
    const flat = (v) => Array.isArray(v) ? v.reduce((a, b) => a + (Number(b) || 0), 0) : v;
    const chip = sys.chip || sys.target || sys.model || "ESP";
    const ver = sys.ver || sys.version || sys.fw || sys.app || "—";
    const up = sys.uptime ?? sys.up ?? sys.up_s;
    const heap = sys.heap ?? sys.heap_free ?? sys.free_heap;
    const mode = st.mode || "—";
    const free = st.free, total = st.total;
    const pending = st.pending ?? "—";

    state.logDebug = lg.lvl === "D";

    // 側欄
    $("#dlChip").textContent = chip;
    $("#dlVer").textContent = ver;
    $("#dlUp").textContent = fmtUptime(up);
    $("#dlHeap").textContent = fmtBytes(heap);
    $("#dlFree").textContent = fmtBytes(free);
    $("#dlTotal").textContent = fmtBytes(total);
    $("#dlPending").textContent = pending;
    $("#dlProduced").textContent = flat(lg.produced) ?? "—";
    $("#dlSent").textContent = flat(lg.sent) ?? "—";
    $("#dlDropped").textContent = flat(lg.dropped) ?? "—";
    $("#sideMode").textContent = mode;
    $("#sideMode").className = "chip " + (mode === "app" ? "led" : "amber");
    $("#ticketName").textContent = chip;
    $("#ticketMeta").textContent = `${state.port || "—"} · ${mode}`;

    // 設備頁
    $("#stChip").textContent = chip;
    $("#stVer").textContent = ver;
    $("#stUptime").textContent = fmtUptime(up);
    $("#stHeap").textContent = fmtBytes(heap);
    $("#stMode").textContent = mode;
    $("#stMode").className = "chip " + (mode === "app" ? "led" : "amber");
    $("#stModeText").textContent = mode;
    $("#stFree").textContent = fmtBytes(free);
    $("#stTotal").textContent = fmtBytes(total);
    $("#stPending").textContent = pending;
    $("#stLogLvl").textContent = lg.lvl ?? "—";
    $("#stLogProd").textContent = flat(lg.produced) ?? "—";
    $("#stLogSent").textContent = flat(lg.sent) ?? "—";
    $("#stLogDrop").textContent = flat(lg.dropped) ?? "—";
    $("#statusRaw").textContent = JSON.stringify(d, null, 2);

    const dm = $("#driveMode");
    if (dm) {
      dm.textContent = mode;
      dm.className = "chip " + (mode === "app" ? "amber" : "led");
    }
    const fm = $("#filesModeBar");
    if (fm) fm.classList.toggle("hidden", !state.connected || mode === "app");
    const dh = $("#driveHint");
    if (dh) {
      dh.textContent = mode === "app"
        ? "裝置持有 FAT。按『交還 FAT』，等電腦見到碟，再揀資料夾。"
        : mode === "usb"
          ? "電腦應該見到碟。揀資料夾 → 拖拉／編輯；完成後按『接管 FAT』或『接管並重啟』。"
          : "FAT 狀態不明；試下 STATUS_GET storage。";
    }

    // 狀態列 / 用量
    $("#stPort").textContent = `port ${state.port || "—"}`;
    $("#stMode2").textContent = `mode ${mode}`;
    $("#stFree2").textContent = `free ${fmtBytes(free)}`;
    if (typeof free === "number" && typeof total === "number" && total > 0) {
      $("#usedMeter").style.width = `${Math.min(100, ((total - free) / total) * 100)}%`;
    }
  } catch (e) {
    toast(`STATUS 失敗：${e.message}`, "err");
  }
}

// ── 檔案 ───────────────────────────────────────────────
function renderCrumb(path) {
  const el = $("#crumb");
  el.innerHTML = "";
  const root = document.createElement("a");
  root.textContent = "/";
  root.onclick = () => loadDir("/");
  el.appendChild(root);
  let acc = "";
  const parts = path.split("/").filter(Boolean);
  parts.forEach((p, i) => {
    acc += "/" + p;
    if (i > 0) {
      const sep = document.createElement("span");
      sep.textContent = "/";
      el.appendChild(sep);
    }
    const a = document.createElement("a");
    a.textContent = p;
    const target = acc;
    a.onclick = () => loadDir(target);
    el.appendChild(a);
  });
}

async function loadManifest({ quiet = false } = {}) {
  if (!state.connected) return false;
  try {
    const j = await api("/api/manifest");
    state.manifest = j.data || {};
    renderCrumb(state.cwd);
    renderFiles(entriesFor(state.cwd));
    return true;
  } catch (e) {
    const msg = String(e.message || "");
    if (msg.includes("not_found") || msg.includes("404")) {
      state.manifest = {};
      renderCrumb(state.cwd);
      renderFiles([]);
      if (!quiet) toast("未建立索引：按『重建索引』", "warn", 5000);
      return false;
    }
    toast(`讀 manifest 失敗：${msg}`, "err", 6000);
    return false;
  }
}

async function rescan() {
  if (!state.connected) { toast("未連線", "warn"); return; }
  try {
    toast("重建索引中…", "warn", 4000);
    logLine("送出 FILE_SCAN(0x200B)…", "info");
    await api("/api/scan", { method: "POST" });
    const before = JSON.stringify(state.manifest || {});
    for (let i = 0; i < 20; i++) {
      await new Promise((r) => setTimeout(r, 500));
      if (!(await loadManifest({ quiet: true }))) continue;
      if (JSON.stringify(state.manifest || {}) !== before) {
        toast("索引完成", "ok");
        logLine("manifest 已更新", "ok");
        return;
      }
    }
    await loadManifest({ quiet: true });
    toast("索引完成（內容無變更）", "ok");
  } catch (e) {
    toast(`重建索引失敗：${e.message}`, "err", 6000);
  }
}

function loadDir(path) {
  state.cwd = path;
  renderCrumb(state.cwd);
  renderFiles(entriesFor(state.cwd));
}

function entriesFor(cwd) {
  const dirs = new Set();
  const files = [];
  for (const [path, info] of Object.entries(state.manifest || {})) {
    if (!path.startsWith("/") || path === "/manifest.json") continue;
    let rel;
    if (cwd === "/") rel = path.slice(1);
    else if (path.startsWith(cwd + "/")) rel = path.slice(cwd.length + 1);
    else continue;
    if (!rel) continue;
    const seg = rel.split("/");
    if (seg.length > 1) dirs.add(seg[0]);
    else files.push({ n: seg[0], d: 0, s: Number(info.s || 0), p: Number(info.p || 0) });
  }
  const dirEntries = [...dirs].sort((a, b) => a.localeCompare(b))
    .map((n) => ({ n, d: 1, s: 0, p: 0 }));
  files.sort((a, b) => a.n.localeCompare(b.n));
  return dirEntries.concat(files);
}

function renderFiles(entries) {
  const body = $("#fileBody");
  body.innerHTML = "";
  if (state.cwd !== "/") {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 4;
    const b = document.createElement("span");
    b.className = "nm";
    b.textContent = "⬆ ..";
    b.onclick = () => {
      const parts = state.cwd.split("/").filter(Boolean);
      parts.pop();
      loadDir("/" + parts.join("/"));
    };
    td.appendChild(b);
    tr.appendChild(td);
    body.appendChild(tr);
  }
  if (!entries.length) {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 4;
    td.className = "muted";
    td.textContent = Object.keys(state.manifest || {}).length
      ? "（空資料夾）"
      : "（未建立索引 — 按右上角『重建索引』）";
    tr.appendChild(td);
    body.appendChild(tr);
  }
  for (const e of entries) {
    const full = joinPath(state.cwd, e.n);
    const tr = document.createElement("tr");

    const tdName = document.createElement("td");
    const nm = document.createElement("span");
    nm.className = "nm";
    nm.textContent = (e.d ? "📁 " : "📄 ") + e.n;
    nm.onclick = () => (e.d ? loadDir(full) : openFile(full));
    tdName.appendChild(nm);
    tr.appendChild(tdName);

    const tdSize = document.createElement("td");
    tdSize.textContent = e.d ? "—" : fmtBytes(e.s);
    tr.appendChild(tdSize);

    const tdState = document.createElement("td");
    if (e.p) {
      const c = document.createElement("span");
      c.className = "chip amber";
      c.textContent = "待確認";
      tdState.appendChild(c);
    }
    tr.appendChild(tdState);

    const tdAct = document.createElement("td");
    if (!e.d) {
      tdAct.appendChild(act("編輯", () => openFile(full)));
      tdAct.appendChild(act("下載", () => downloadFile(full)));
      if (e.p) {
        tdAct.appendChild(act("確認", () => fileOp("confirm", full), "ok"));
        tdAct.appendChild(act("復原", () => fileOp("undo", full), "wn"));
      }
      tdAct.appendChild(act("刪除", () => fileOp("delete", full), "er"));
    } else {
      tdAct.appendChild(act("進入", () => loadDir(full)));
    }
    tr.appendChild(tdAct);
    body.appendChild(tr);
  }
}

function act(text, fn, cls = "") {
  const s = document.createElement("span");
  s.className = `act ${cls}`;
  s.textContent = text;
  s.onclick = fn;
  return s;
}

async function openFile(path) {
  try {
    const r = await fetch(`/api/file?path=${encodeURIComponent(path)}`);
    if (!r.ok) {
      let msg = `${r.status}`;
      try { const j = await r.json(); msg = JSON.stringify(j.detail ?? j); } catch (_) { /* */ }
      throw new Error(msg);
    }
    const buf = new Uint8Array(await r.arrayBuffer());
    const text = decodeText(buf);
    if (text === null) {
      toast("二進位檔：改為下載", "warn");
      downloadFile(path);
      return;
    }
    state.file = path;
    state.dirty = false;
    $("#fileBrowser").classList.add("hidden");
    $("#editorCard").classList.remove("hidden");
    $("#editorTitle").textContent = path;
    $("#editorMeta").textContent = fmtBytes(buf.length);
    $("#editor").value = text;
  } catch (e) {
    toast(`開檔失敗：${e.message}`, "err", 6000);
  }
}

async function saveFile() {
  if (!state.file) return;
  const data = new TextEncoder().encode($("#editor").value);
  try {
    setProgress("#fileProgress", "#fileProgressWrap", 0, data.length);
    await putRaw(state.file, data, (done, total) =>
      setProgress("#fileProgress", "#fileProgressWrap", done, total));
    state.dirty = false;
    toast(`已儲存 ${state.file}`, "ok");
    logLine(`已儲存 ${state.file}（${fmtBytes(data.length)}）`, "ok");
    await rescan();
    await refreshStatus();
  } catch (e) {
    toast(`儲存失敗：${e.message}`, "err", 6000);
    logLine(`儲存失敗：${e.message}`, "err");
  } finally {
    setTimeout(() => $("#fileProgressWrap").classList.add("hidden"), 900);
  }
}

function downloadFile(path) {
  const a = document.createElement("a");
  a.href = `/api/file?path=${encodeURIComponent(path)}`;
  a.download = baseName(path);
  document.body.appendChild(a);
  a.click();
  a.remove();
  logLine(`下載 ${path}`, "ok");
}

async function fileOp(op, path) {
  const label = { delete: "刪除", confirm: "確認", undo: "復原" }[op] || op;
  if (op === "delete" && !confirm(`刪除 ${path}？`)) return;
  try {
    await api(`/api/file/${op}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path }),
    });
    toast(`${label}完成：${path}`, "ok");
    logLine(`${label} ${path}`, "ok");
    if (state.file === path) closeEditor();
    await rescan();
    await refreshStatus();
  } catch (e) {
    toast(`${label}失敗：${e.message}`, "err", 6000);
  }
}

// ── 上傳 ───────────────────────────────────────────────
function putRaw(path, data, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("PUT", `/api/file?path=${encodeURIComponent(path)}`);
    xhr.responseType = "json";
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) onProgress(e.loaded, e.total);
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) resolve(xhr.response);
      else reject(new Error(JSON.stringify(xhr.response?.detail ?? xhr.statusText)));
    };
    xhr.onerror = () => reject(new Error("網絡錯誤"));
    xhr.send(data);
  });
}

async function uploadFiles(files) {
  if (!state.connected) { toast("未連線", "warn"); return; }
  const list = Array.from(files).filter((f) => f.name && !f.name.includes("/"));
  if (!list.length) return;
  for (const f of list) {
    const path = joinPath(state.cwd, f.name);
    try {
      logLine(`上傳 ${f.name} → ${path}（${fmtBytes(f.size)}）`, "info");
      const buf = new Uint8Array(await f.arrayBuffer());
      const res = await putRaw(path, buf, (done, total) =>
        setProgress("#fileProgress", "#fileProgressWrap", done, total));
      const extra = res?.auto_confirmed ? "（已自動確認）" : (res?.pending ? "（待確認）" : "");
      toast(`上傳完成：${f.name}${extra}`, "ok");
      logLine(`上傳完成 ${path} ${fmtBytes(res?.size)} sha=${(res?.sha256 || "").slice(0, 12)}…`, "ok");
    } catch (e) {
      toast(`上傳失敗 ${f.name}：${e.message}`, "err", 7000);
      logLine(`上傳失敗 ${f.name}：${e.message}`, "err");
    }
  }
  $("#filePicker").value = "";
  $("#fileProgressWrap").classList.add("hidden");
  await rescan();
  await refreshStatus();
}

function setProgress(barSel, wrapSel, done, total) {
  const wrap = $(wrapSel);
  if (!wrap) return;
  wrap.classList.remove("hidden");
  const pct = total > 0 ? Math.min(100, (done / total) * 100) : 0;
  $(barSel).style.width = pct.toFixed(1) + "%";
}

// ── 隨身碟（MSC 捷徑）──────────────────────────────────
async function loadDrives() {
  try {
    const j = await api("/api/drives");
    const sel = $("#drivePick");
    const keep = sel.value;
    sel.innerHTML = "";
    for (const d of j.data || []) {
      const o = document.createElement("option");
      o.value = d.path;
      o.textContent = `${d.name}  ${d.path}`;
      sel.appendChild(o);
    }
    const custom = document.createElement("option");
    custom.value = "__custom";
    custom.textContent = "（自訂路徑…）";
    sel.appendChild(custom);
    if (keep) sel.value = keep;
  } catch (e) {
    console.warn("drives:", e);
  }
}

function useDrive() {
  let root = $("#drivePick").value;
  if (!root) { toast("先揀一個資料夾", "warn"); return; }
  if (root === "__custom") {
    root = prompt("資料夾路徑（例如 D:\\ 或模擬碟 .simfs 的絕對路徑）", state.driveRoot || "D:\\");
    if (!root) return;
  }
  state.driveRoot = root.trim();
  state.drivePath = "/";
  loadDrive("/");
}

function driveCrumb(path) {
  const el = $("#driveCrumb");
  el.innerHTML = "";
  const root = document.createElement("a");
  const parts0 = (state.driveRoot || "碟").split(/[\\/]/).filter(Boolean);
  root.textContent = (state.driveRoot || "").endsWith("\\") || (state.driveRoot || "").endsWith("/")
    ? (state.driveRoot || "").slice(0, 2)
    : (parts0[parts0.length - 1] || state.driveRoot);
  root.title = state.driveRoot;
  root.onclick = () => loadDrive("/");
  el.appendChild(root);
  let acc = "";
  const parts = path.split("/").filter(Boolean);
  parts.forEach((p, i) => {
    acc += "/" + p;
    if (i > 0) {
      const sep = document.createElement("span");
      sep.textContent = "/";
      el.appendChild(sep);
    }
    const a = document.createElement("a");
    a.textContent = p;
    const target = acc;
    a.onclick = () => loadDrive(target);
    el.appendChild(a);
  });
}

async function loadDrive(path) {
  if (!state.driveRoot) { toast("先揀資料夾", "warn"); return; }
  try {
    const j = await api(`/api/drive/list?root=${encodeURIComponent(state.driveRoot)}` +
                        `&path=${encodeURIComponent(path)}`);
    state.drivePath = j.data.path || path;
    driveCrumb(state.drivePath);
    renderDriveFiles(j.data.entries || []);
  } catch (e) {
    toast(`讀資料夾失敗：${e.message}`, "err", 7000);
  }
}

function renderDriveFiles(entries) {
  const body = $("#driveBody");
  body.innerHTML = "";
  if (state.drivePath !== "/") {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 3;
    const b = document.createElement("span");
    b.className = "nm";
    b.textContent = "⬆ ..";
    b.onclick = () => {
      const parts = state.drivePath.split("/").filter(Boolean);
      parts.pop();
      loadDrive("/" + parts.join("/"));
    };
    td.appendChild(b);
    tr.appendChild(td);
    body.appendChild(tr);
  }
  if (!entries.length) {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 3;
    td.className = "muted";
    td.textContent = "（空資料夾）";
    tr.appendChild(td);
    body.appendChild(tr);
  }
  for (const e of entries) {
    const full = joinPath(state.drivePath, e.n);
    const tr = document.createElement("tr");

    const tdName = document.createElement("td");
    const nm = document.createElement("span");
    nm.className = "nm";
    nm.textContent = (e.d ? "📁 " : "📄 ") + e.n;
    nm.onclick = () => (e.d ? loadDrive(full) : openDriveFile(full, e.n));
    tdName.appendChild(nm);
    tr.appendChild(tdName);

    const tdSize = document.createElement("td");
    tdSize.textContent = e.d ? "—" : fmtBytes(e.s);
    tr.appendChild(tdSize);

    const tdAct = document.createElement("td");
    if (e.d) {
      tdAct.appendChild(act("進入", () => loadDrive(full)));
      tdAct.appendChild(act("刪除", () => deleteDrive(full, true), "er"));
    } else {
      tdAct.appendChild(act("編輯", () => openDriveFile(full, e.n)));
      tdAct.appendChild(act("下載", () => downloadDrive(full)));
      tdAct.appendChild(act("刪除", () => deleteDrive(full, false), "er"));
    }
    tr.appendChild(tdAct);
    body.appendChild(tr);
  }
}

async function openDriveFile(path, name) {
  try {
    const r = await fetch(`/api/drive/file?root=${encodeURIComponent(state.driveRoot)}` +
                          `&path=${encodeURIComponent(path)}`);
    if (!r.ok) throw new Error(`${r.status} ${r.statusText}`);
    const buf = new Uint8Array(await r.arrayBuffer());
    const text = decodeText(buf);
    if (text === null) {
      toast("二進位檔：改為下載", "warn");
      downloadDrive(path);
      return;
    }
    state.driveFile = path;
    state.driveDirty = false;
    $("#driveEditorCard").classList.remove("hidden");
    $("#driveEditorTitle").textContent = path;
    $("#driveEditorMeta").textContent = fmtBytes(buf.length);
    $("#driveEditor").value = text;
  } catch (e) {
    toast(`開檔失敗：${e.message}`, "err", 6000);
  }
}

async function saveDriveFile() {
  if (!state.driveFile) return;
  const data = new TextEncoder().encode($("#driveEditor").value);
  try {
    await putDrive(state.driveFile, data, (done, total) =>
      setProgress("#driveProgress", "#driveProgressWrap", done, total));
    state.driveDirty = false;
    toast(`已寫入 ${state.driveFile}`, "ok");
    logLine(`隨身碟：已寫入 ${state.driveFile}（${fmtBytes(data.length)}）`, "ok");
    await loadDrive(state.drivePath);
  } catch (e) {
    toast(`寫入失敗：${e.message}`, "err", 7000);
  } finally {
    setTimeout(() => $("#driveProgressWrap").classList.add("hidden"), 800);
  }
}

function closeDriveEditor() {
  if (state.driveDirty && !confirm("有未儲存修改，關閉？")) return;
  state.driveFile = null;
  state.driveDirty = false;
  $("#driveEditorCard").classList.add("hidden");
  $("#driveEditor").value = "";
}

function downloadDrive(path) {
  const a = document.createElement("a");
  a.href = `/api/drive/file?root=${encodeURIComponent(state.driveRoot)}` +
           `&path=${encodeURIComponent(path)}`;
  a.download = baseName(path);
  document.body.appendChild(a);
  a.click();
  a.remove();
  logLine(`隨身碟：下載 ${path}`, "ok");
}

async function deleteDrive(path, isDir) {
  if (!confirm(`刪除 ${path}${isDir ? "（整個資料夾）" : ""}？`)) return;
  try {
    await api("/api/drive/delete", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ root: state.driveRoot, path, recursive: isDir ? 1 : 0 }),
    });
    toast(`已刪除 ${path}`, "ok");
    if (state.driveFile === path) closeDriveEditor();
    await loadDrive(state.drivePath);
  } catch (e) {
    toast(`刪除失敗：${e.message}`, "err", 7000);
  }
}

function putDrive(path, data, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open("PUT", `/api/drive/file?root=${encodeURIComponent(state.driveRoot)}` +
                    `&path=${encodeURIComponent(path)}`);
    xhr.responseType = "json";
    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable && onProgress) onProgress(e.loaded, e.total);
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) resolve(xhr.response);
      else reject(new Error(JSON.stringify(xhr.response?.detail ?? xhr.statusText)));
    };
    xhr.onerror = () => reject(new Error("網絡錯誤"));
    xhr.send(data);
  });
}

async function uploadDrive(files) {
  if (!state.driveRoot) { toast("先揀資料夾", "warn"); return; }
  const list = Array.from(files).filter((f) => f.name && !f.name.includes("/"));
  for (const f of list) {
    const path = joinPath(state.drivePath, f.name);
    try {
      const buf = new Uint8Array(await f.arrayBuffer());
      await putDrive(path, buf, (done, total) =>
        setProgress("#driveProgress", "#driveProgressWrap", done, total));
      toast(`已寫入 ${f.name}`, "ok");
      logLine(`隨身碟：寫入 ${path}（${fmtBytes(f.size)}）`, "ok");
    } catch (e) {
      toast(`寫入失敗 ${f.name}：${e.message}`, "err", 7000);
    }
  }
  $("#driveProgressWrap").classList.add("hidden");
  await loadDrive(state.drivePath);
}

async function mountDrive() {
  if (!state.connected) { toast("未連線", "warn"); return; }
  await statusSet({ System: { file_mode: 0 } }, "交還 FAT");
  $("#driveHint").textContent =
    "等 1–2 秒，電腦應該見到碟（D: / E:）。按上面『⟳』再揀個碟，然後『用』。";
  logLine("已交還 FAT：等電腦掛載隨身碟…", "warn");
  setTimeout(loadDrives, 1500);
}

async function claimDrive() {
  if (!state.connected) { toast("未連線", "warn"); return; }
  await statusSet({ System: { file_mode: 1 } }, "接管 FAT");
  $("#driveHint").textContent = "已接管 FAT：裝置見到新檔案。要即時生效就按『接管並重啟』。";
}

async function applyAndReboot() {
  if (!state.connected) { toast("未連線", "warn"); return; }
  if (!confirm("接管 FAT 並重啟？連線會斷，之後要重新連線。")) return;
  await statusSet({ System: { file_mode: 1 } }, "接管 FAT");
  try {
    await api("/api/reboot", { method: "POST" });
    setConnected(false);
    toast("已送出重啟；板子回來後重新連線", "warn", 6000);
  } catch (e) {
    toast(`重啟失敗：${e.message}`, "err");
  }
}

// ── OTA ───────────────────────────────────────────────
async function pickFirmware(file) {
  if (!file) return;
  state.fwFile = file;
  const info = $("#fwInfo");
  info.classList.remove("hidden");
  info.textContent = `${file.name}\n${fmtBytes(file.size)}\n計算 SHA-256…`;
  $("#btnOta").disabled = false;
  try {
    const buf = await file.arrayBuffer();
    const digest = await crypto.subtle.digest("SHA-256", buf);
    const hex = Array.from(new Uint8Array(digest)).map((b) => b.toString(16).padStart(2, "0")).join("");
    info.textContent = `${file.name}\n${fmtBytes(file.size)} · sha256 ${hex}`;
  } catch (_) {
    info.textContent = `${file.name}\n${fmtBytes(file.size)}`;
  }
}

function chipFromName(name) {
  const n = (name || "").toLowerCase();
  if (n.includes("p4")) return "esp32p4";
  if (n.includes("s3") || n.includes("matrix")) return "esp32s3";
  if (n.includes("c3")) return "esp32c3";
  return "";
}

async function startOta() {
  if (!state.fwFile) return;

  // 安全鎖：S3/P4 都係 303A:4003；先對 sys.chip，唔啱就唔好推。
  const expected = chipFromName(state.fwFile.name);
  let actual = "";
  try {
    const st = await api("/api/status?keys=sys");
    actual = String(st.data?.sys?.chip || "").toLowerCase();
  } catch (_) {
    actual = "";
  }
  if (actual && expected && actual.replace(/[-_]/g, "") !== expected.replace(/[-_]/g, "")) {
    toast(`板子係 ${actual}，image 係 ${expected} — 拒絕 OTA`, "err", 9000);
    logLine(`OTA 拒絕：板子 ${actual} ≠ image ${expected}`, "err");
    return;
  }
  if (!actual) {
    if (!confirm("讀唔到 sys.chip（可能係舊 firmware）。\n真係確定 image 同板型一致？")) return;
  }
  if (!confirm(`開始 OTA：${state.fwFile.name}（${fmtBytes(state.fwFile.size)}）？\n燒錄期間唔好拔線。`)) return;

  const apply = $("#fwApply").checked ? 1 : 0;
  const q = `apply=${apply}` + (expected ? `&expect_chip=${expected}` : "");
  const xhr = new XMLHttpRequest();
  xhr.open("POST", `/api/firmware?${q}`);
  xhr.responseType = "json";
  $("#btnOta").disabled = true;
  $("#led").dataset.state = "busy";
  $("#otaResult").classList.remove("hidden");
  $("#otaResult").textContent = "上傳中…";
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) setProgress("#otaProgress", "#otaProgressWrap", e.loaded, e.total);
  };
  xhr.onload = () => {
    $("#btnOta").disabled = false;
    $("#led").dataset.state = state.connected ? "idle" : "error";
    if (xhr.status >= 200 && xhr.status < 300) {
      const d = xhr.response?.data || {};
      $("#otaResult").textContent = JSON.stringify(d, null, 2);
      toast("OTA 完成", "ok", 5000);
      logLine(`OTA 完成：${d.size} B，${d.seconds}s`, "ok");
      if (apply) {
        setConnected(false);
        logLine("板子重啟中，CDC 會短暫消失；完成後重新連線。", "warn");
      }
    } else {
      const msg = JSON.stringify(xhr.response?.detail ?? xhr.statusText);
      $("#otaResult").textContent = `失敗：${msg}`;
      toast(`OTA 失敗：${msg}`, "err", 8000);
      logLine(`OTA 失敗：${msg}`, "err");
    }
    setTimeout(() => $("#otaProgressWrap").classList.add("hidden"), 900);
  };
  xhr.onerror = () => {
    $("#btnOta").disabled = false;
    toast("OTA 網絡錯誤", "err");
  };
  xhr.send(state.fwFile);
}

// ── Console / quick command ───────────────────────────
async function loadCommands() {
  try {
    const j = await api("/api/commands");
    state.commands = j.data || [];
    const sel = $("#cmdSelect");
    sel.innerHTML = "";
    for (const c of state.commands) {
      const o = document.createElement("option");
      o.value = c.cmd;
      o.textContent = `${c.cmd}  ${c.name}`;
      sel.appendChild(o);
    }
    sel.value = "0x1101";
  } catch (e) {
    console.warn("commands:", e);
  }
}

async function sendCmd(cmd, payloadHex, expect, timeout = 5) {
  const body = {
    cmd,
    payload_hex: payloadHex || "",
    expect: expect || null,
    timeout: Number(timeout) || 5,
  };
  return api("/api/cmd", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

async function onSendCmd() {
  const cmd = $("#cmdSelect").value || "0x1101";
  let payload;
  try { payload = hexToBytes($("#cmdPayload").value); }
  catch (e) { toast(e.message, "err"); return; }
  const expText = $("#cmdExpect").value.trim();
  const expect = expText ? expText.split(",").map((s) => s.trim()).filter(Boolean) : null;
  const hex = Array.from(payload).map((b) => b.toString(16).padStart(2, "0")).join("");
  $("#cmdResult").textContent = "等待回覆…";
  try {
    const j = await sendCmd(cmd, hex, expect, $("#cmdTimeout").value);
    const jsonText = j.json ? JSON.stringify(j.json, null, 2) : "（唔係 JSON）";
    $("#cmdResult").textContent = `cmd ${j.cmd}  len ${j.len}\n${jsonText}\n\nhex:\n${j.hex}`;
    logLine(`cmd ${cmd} → ${j.cmd}（${j.len} B）`, "ok");
  } catch (e) {
    $("#cmdResult").textContent = `失敗：${e.message}`;
    logLine(`cmd ${cmd} 失敗：${e.message}`, "err");
  }
}

async function onQuickSend() {
  const raw = $("#quickCmd").value.trim();
  let cmd = "0x1101";
  let payload = "";
  if (raw) {
    const parts = raw.split(/\s+/);
    if (!/^0x[0-9a-fA-F]+$/.test(parts[0])) {
      toast("格式：0xCMD [hex payload]", "warn");
      return;
    }
    cmd = parts[0];
    payload = (parts[1] || "").replace(/[^0-9a-fA-F]/g, "");
  }
  try {
    const j = await sendCmd(cmd, payload, null, 6);
    logLine(`>>> ${cmd} ${payload}`.trim(), "dim");
    if (j.json) logLine(JSON.stringify(j.json, null, 2), "ok");
    else logLine(`${j.cmd}  ${j.len} B  ${j.hex}`, "ok");
    $("#quickCmd").value = "";
  } catch (e) {
    logLine(`>>> ${cmd} 失敗：${e.message}`, "err");
  }
}

function parseStatusAck(hex) {
  const clean = (hex || "").replace(/[^0-9a-fA-F]/g, "");
  const b = new Uint8Array(clean.length / 2);
  for (let i = 0; i < b.length; i++) b[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16);
  if (b.length < 3) return { ok: false, message: "" };
  const n = b[1] | (b[2] << 8);
  const msg = new TextDecoder("utf-8").decode(b.slice(3, 3 + Math.min(n, b.length - 3)));
  return { ok: b[0] === 1, message: msg };
}

function statusSet(obj, label, persist = 0) {
  const json = new TextEncoder().encode(JSON.stringify(obj));
  const payload = new Uint8Array(2 + json.length + 1);
  payload[0] = json.length & 0xFF;
  payload[1] = (json.length >> 8) & 0xFF;
  payload.set(json, 2);
  payload[2 + json.length] = persist ? 1 : 0;
  const hex = Array.from(payload).map((b) => b.toString(16).padStart(2, "0")).join("");
  return sendCmd("0x1103", hex, ["0x1104"], 5)
    .then((j) => {
      const ack = parseStatusAck(j.hex);
      const msg = ack.message || JSON.stringify(j.json ?? {});
      toast(`${label}：${msg}`, ack.ok ? "ok" : "err", 5000);
      logLine(`${label} → ${msg}`, ack.ok ? "ok" : "err");
      return refreshStatus();
    })
    .catch((e) => {
      toast(`${label}失敗：${e.message}`, "err", 7000);
    });
}

// ── 版面 ───────────────────────────────────────────────
function switchView(name) {
  state.view = name;
  $$(".rail .nav[data-view]").forEach((b) =>
    b.setAttribute("aria-current", b.dataset.view === name ? "true" : "false"));
  $$(".view").forEach((v) => v.dataset.on = v.dataset.view === name ? "true" : "false");
  if (name === "files" && state.connected) {
    if (!Object.keys(state.manifest || {}).length) loadManifest();
    else { renderCrumb(state.cwd); renderFiles(entriesFor(state.cwd)); }
  }
  if (name === "drive" && state.driveRoot) loadDrive(state.drivePath);
}

function bindUI() {
  $$(".rail .nav[data-view]").forEach((b) => b.onclick = () => switchView(b.dataset.view));

  $("#btnConnect").onclick = () => {
    const port = $("#portSelect").value;
    if (!port) { toast("冇 port：插線或按『模擬裝置』", "warn"); return; }
    connect(port);
  };
  $("#btnSim").onclick = () => connect("sim");
  $("#btnDisconnect").onclick = disconnect;
  $("#btnReboot").onclick = async () => {
    if (!confirm("重啟裝置？連線會斷。")) return;
    try {
      await api("/api/reboot", { method: "POST" });
      toast("已送出 REBOOT", "ok");
      logLine("已送出 REBOOT", "warn");
    } catch (e) { toast(`重啟失敗：${e.message}`, "err"); }
  };

  $("#btnClaimFat").onclick = () => statusSet({ System: { file_mode: 1 } }, "接管 FAT");
  $("#btnReleaseFat").onclick = () => statusSet({ System: { file_mode: 0 } }, "交還 host");
  $("#btnMountDrive").onclick = mountDrive;
  $("#btnClaimDrive").onclick = claimDrive;
  $("#btnApplyReboot").onclick = applyAndReboot;
  $("#btnDriveUse").onclick = useDrive;
  $("#btnRefreshDrive").onclick = () => {
    loadDrives();
    if (state.driveRoot) loadDrive(state.drivePath);
  };
  $("#btnDriveSave").onclick = saveDriveFile;
  $("#btnDriveClose").onclick = closeDriveEditor;
  $("#driveEditor").addEventListener("input", () => { state.driveDirty = true; });

  const dd = $("#driveDrop");
  dd.addEventListener("dragover", (e) => { e.preventDefault(); dd.classList.add("hot"); });
  dd.addEventListener("dragleave", () => dd.classList.remove("hot"));
  dd.addEventListener("drop", (e) => {
    e.preventDefault();
    dd.classList.remove("hot");
    uploadDrive(e.dataTransfer.files);
  });
  $("#btnLogDebug").onclick = () =>
    statusSet({ System: { log_debug_mode: state.logDebug ? 0 : 1 } },
              state.logDebug ? "正常 log" : "除錯 log");
  $("#btnRefreshStatus").onclick = refreshStatus;

  $("#btnRefreshFiles").onclick = () => loadManifest();
  $("#btnScan").onclick = rescan;
  $("#btnFilesClaim").onclick = () =>
    statusSet({ System: { file_mode: 1 } }, "接管 FAT")
      .then(() => loadManifest());
  $("#btnUpload").onclick = () => $("#filePicker").click();
  $("#filePicker").onchange = (e) => uploadFiles(e.target.files);

  const dz = $("#dropzone");
  dz.addEventListener("dragover", (e) => { e.preventDefault(); dz.classList.add("hot"); });
  dz.addEventListener("dragleave", () => dz.classList.remove("hot"));
  dz.addEventListener("drop", (e) => {
    e.preventDefault();
    dz.classList.remove("hot");
    uploadFiles(e.dataTransfer.files);
  });

  $("#btnSaveFile").onclick = saveFile;
  $("#btnDownloadFile").onclick = () => state.file && downloadFile(state.file);
  $("#btnCloseEditor").onclick = closeEditor;
  $("#editor").addEventListener("input", () => { state.dirty = true; });

  $("#btnPickFw").onclick = () => $("#fwPicker").click();
  $("#fwPicker").onchange = (e) => pickFirmware(e.target.files[0]);
  const fw = $("#fwDrop");
  fw.addEventListener("dragover", (e) => { e.preventDefault(); fw.classList.add("hot"); });
  fw.addEventListener("dragleave", () => fw.classList.remove("hot"));
  fw.addEventListener("drop", (e) => {
    e.preventDefault();
    fw.classList.remove("hot");
    pickFirmware(e.dataTransfer.files[0]);
  });
  $("#btnOta").onclick = startOta;

  $("#btnSendCmd").onclick = onSendCmd;
  $("#btnQuickSend").onclick = onQuickSend;
  $("#quickCmd").addEventListener("keydown", (e) => { if (e.key === "Enter") onQuickSend(); });

  $("#logPause").onchange = (e) => { state.logPaused = e.target.checked; };
  $("#btnClearLog").onclick = () => { $("#terminal").innerHTML = ""; state.logs = []; };
  $("#btnSaveLog").onclick = () => {
    const blob = new Blob([state.logs.join("\n")], { type: "text/plain" });
    const a = document.createElement("a");
    a.href = URL.createObjectURL(blob);
    a.download = "esp-webmaster.log";
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 3000);
  };

  // 底部面板高度拖曳
  const grip = $("#grip");
  let dragging = false;
  grip.addEventListener("mousedown", () => { dragging = true; document.body.style.cursor = "row-resize"; });
  window.addEventListener("mouseup", () => { dragging = false; document.body.style.cursor = ""; });
  window.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    const h = Math.max(90, Math.min(window.innerHeight * 0.6, window.innerHeight - e.clientY - 28));
    document.documentElement.style.setProperty("--panel-h", `${h}px`);
  });

  // 快捷鍵：Ctrl/Cmd-1..5 切 view
  window.addEventListener("keydown", (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    const map = { "1": "device", "2": "files", "3": "drive", "4": "flash", "5": "monitor" };
    if (map[e.key]) { e.preventDefault(); switchView(map[e.key]); }
  });

  window.addEventListener("beforeunload", (e) => {
    if (state.dirty || state.driveDirty) { e.preventDefault(); e.returnValue = ""; }
  });
}

function closeEditor() {
  if (state.dirty && !confirm("有未儲存修改，關閉？")) return;
  state.file = null;
  state.dirty = false;
  $("#editorCard").classList.add("hidden");
  $("#fileBrowser").classList.remove("hidden");
  $("#editor").value = "";
}

// ── 啟動 ───────────────────────────────────────────────
async function init() {
  bindUI();
  loadCommands();
  refreshPorts();
  loadDrives();
  setInterval(() => { if (!state.connected) refreshPorts(); }, 4000);

  logLine("ESP WebMaster 已啟動。揀 port → 連線，或按『模擬裝置』。", "info");
  try {
    const j = await api("/api/info");
    if (j.connected) {
      setConnected(true, j.port, j.sim);
      openWS();
      await refreshStatus();
      if (!(await loadManifest({ quiet: true }))) await rescan();
    }
  } catch (e) {
    logLine(`伺服器狀態讀取失敗：${e.message}`, "err");
  }
}

document.addEventListener("DOMContentLoaded", init);
