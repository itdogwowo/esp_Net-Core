/* ESP WebMaster — 檔案頁：manifest、上傳／下載／刪除
 * CDC FAT 的檔案操作（FILE_SCAN／BEGIN／CHUNK／END／CONFIRM…）。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

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
    ideTreeRender();
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
    xhr.setRequestHeader("X-Actor", "ui");
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
