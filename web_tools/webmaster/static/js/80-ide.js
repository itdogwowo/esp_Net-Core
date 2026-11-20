/* ESP WebMaster — IDE：檔案樹、多 tab、CodeMirror、lint／格式化
 * IDE 的主要邏輯；容器檢視器另存 85-container.js。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

// ═══════════════════════ IDE ═══════════════════════
function ideExtInfo(path) {
  const ext = String(path || "").split(".").pop().toLowerCase();
  const map = {
    json: { cls: "json", label: "{}" },
    js: { cls: "js", label: "JS" }, mjs: { cls: "js", label: "JS" },
    c: { cls: "c", label: "C" }, h: { cls: "c", label: "H" },
    cpp: { cls: "c", label: "C+" }, hpp: { cls: "c", label: "H+" },
    py: { cls: "py", label: "PY" },
    md: { cls: "md", label: "MD" }, markdown: { cls: "md", label: "MD" },
    sh: { cls: "sh", label: "SH" }, bash: { cls: "sh", label: "SH" },
    bin: { cls: "bin", label: "BIN" }, nct: { cls: "bin", label: "NCT" },
    nce: { cls: "bin", label: "NCE" }, nca: { cls: "bin", label: "NCA" },
    txt: { cls: "txt", label: "TXT" },
  };
  return map[ext] || { cls: "txt", label: (ext.slice(0, 3) || "•").toUpperCase() };
}

function ideModeFor(path) {
  const p = String(path).toLowerCase();
  if (p.endsWith(".json") || p.endsWith(".js") || p.endsWith(".mjs")) return "application/json";
  if (p.endsWith(".py")) return "text/x-python";
  if (/\.(c|h|cpp|hpp|cc)$/.test(p)) return "text/x-csrc";
  if (/\.(xml|html|htm|svg)$/.test(p)) return "application/xml";
  if (p.endsWith(".md") || p.endsWith(".markdown")) return "text/x-markdown";
  if (/\.(sh|bash|zsh)$/.test(p)) return "text/x-sh";
  return "text/plain";
}

function ideActiveTab() {
  return state.ideTabs.find((t) => t.path === state.ideActive) || null;
}

function ideEnsureCM() {
  if (state.ideCM) return state.ideCM;
  if (!window.CodeMirror) {
    toast("CodeMirror 未載入：只可以用純文字編輯", "warn", 6000);
    return null;
  }
  const cm = window.CodeMirror.fromTextArea($("#ideEditor"), {
    lineNumbers: true, lineWrapping: false, indentUnit: 2, tabSize: 2,
    mode: "text/plain", autoCloseBrackets: true, matchBrackets: true,
    gutters: ["CodeMirror-lint-markers"],
    viewportMargin: 40,
  });
  cm.setSize("100%", "100%");
  cm.setOption("lint", { getAnnotations: () => ideLintAnnotations(cm) });
  let savedTheme = "bench";
  try { savedTheme = localStorage.getItem("espwm.ideTheme") || "bench"; } catch (_) { /* */ }
  ideApplyTheme(savedTheme);
  cm.on("change", () => {
    if (state.ideSilent) return;
    const tab = ideActiveTab();
    if (!tab) return;
    tab.text = cm.getValue();
    if (!tab.dirty) {
      tab.dirty = true;
      ideRenderTabs();
    }
    ideSyncBar();
    ideLintStatus();
  });
  cm.on("cursorActivity", ideSyncBar);
  state.ideCM = cm;
  return cm;
}

function ideSyncBar() {
  const tab = ideActiveTab();
  const cm = state.ideCM;
  $("#idePath").textContent = tab ? tab.path : "未開啟檔案";
  $("#ideDirty").classList.toggle("hidden", !(tab && tab.dirty));
  const binary = !!(tab && tab.binary);
  $("#btnIdeSave").disabled = !tab || binary;
  $("#btnIdeConfirm").disabled = !tab;
  $("#btnIdeUndo").disabled = !tab;
  $("#btnIdeDownload").disabled = !tab;
  $("#btnIdeWave").disabled = !tab || binary;   // 波形模擬器：文字檔先可以解析
  $("#btnIdeDelete").disabled = !tab;
  $("#btnIdeFormat").disabled = !tab || binary;
  $("#btnIdeValidate").disabled = !tab || binary;
  if (!tab) $("#ideLint").classList.add("hidden");
  if (binary) {
    $("#ideMode").textContent = "NCM 容器";
    $("#ideCursor").textContent = "—";
    $("#ideSize").textContent = fmtBytes(tab.container?.file?.size || 0);
  } else {
    $("#ideMode").textContent = tab ? (tab.mode || "text/plain") : "—";
    if (cm && tab) {
      const c = cm.getCursor();
      $("#ideCursor").textContent = `Ln ${c.line + 1}, Col ${c.ch + 1}`;
      $("#ideSize").textContent = `${cm.getValue().length} B`;
    } else {
      $("#ideCursor").textContent = "—";
      $("#ideSize").textContent = "—";
    }
  }
}

function ideRenderTabs() {
  const box = $("#ideTabs");
  if (!box) return;
  box.innerHTML = "";
  for (const tab of state.ideTabs) {
    const el = document.createElement("div");
    const ext = ideExtInfo(tab.path);
    el.className = "ide-tab ext-" + ext.cls + (tab.path === state.ideActive ? " active" : "");
    el.title = tab.path;
    const dot = document.createElement("span");
    dot.className = "tab-dot";
    el.appendChild(dot);
    const name = document.createElement("span");
    name.textContent = baseName(tab.path);
    el.appendChild(name);
    if (tab.dirty) {
      const d = document.createElement("span");
      d.className = "dot";
      el.appendChild(d);
    }
    const x = document.createElement("span");
    x.className = "x";
    x.textContent = "×";
    x.onclick = (e) => { e.stopPropagation(); ideClose(tab.path); };
    el.appendChild(x);
    el.onclick = () => ideActivate(tab.path);
    box.appendChild(el);
  }
  ideSyncBar();
  ideTreeActive();
}

function ideTreeActive() {
  $$("#ideTree .ide-node").forEach((n) =>
    n.classList.toggle("active", n.dataset.path === state.ideActive));
}
function ideActivate(path) {
  const tab = state.ideTabs.find((t) => t.path === path);
  if (!tab) return;
  state.ideActive = path;
  const isC = !!tab.container;
  $("#ideContainer").classList.toggle("hidden", !isC);
  $("#ideEditorWrap").classList.toggle("view-container", isC);
  if (isC) {
    ideRenderContainer(tab.container);
    $("#ideEmpty").classList.add("hidden");
  } else {
    const cm = ideEnsureCM();
    if (cm) {
      state.ideSilent = true;
      cm.setValue(tab.text);
      cm.setOption("mode", tab.mode || "text/plain");
      state.ideSilent = false;
      $("#ideEmpty").classList.add("hidden");
      cm.focus();
    }
  }
  ideRenderTabs();
  ideSyncBar();
  ideLintStatus();
}

async function ideOpen(path) {
  const existing = state.ideTabs.find((t) => t.path === path);
  if (existing) { ideActivate(path); return; }
  try {
    // .nct／.nce／.nca：唔開文字編輯器，開容器檢視器（header／manifest／meta）
    if (isContainerPath(path)) {
      const data = await fetchContainer(path);
      state.ideTabs.push({ path, text: "", dirty: false, mode: "text/plain",
                           binary: true, container: data });
      ideActivate(path);
      return;
    }
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
    state.ideTabs.push({ path, text, dirty: false, mode: ideModeFor(path) });
    ideActivate(path);
  } catch (e) {
    toast(`開檔失敗：${e.message}`, "err", 6000);
  }
}

function ideClose(path) {
  const idx = state.ideTabs.findIndex((t) => t.path === path);
  if (idx < 0) return;
  if (state.ideTabs[idx].dirty && !confirm(`${baseName(path)} 有未儲存修改，關閉？`)) return;
  state.ideTabs.splice(idx, 1);
  if (state.ideActive === path) {
    const next = state.ideTabs[Math.max(0, idx - 1)] || state.ideTabs[0];
    state.ideActive = next ? next.path : null;
    const cm = ideEnsureCM();
    if (next) {
      ideActivate(next.path);
    } else {
      if (cm) { state.ideSilent = true; cm.setValue(""); state.ideSilent = false; }
      $("#ideContainer").classList.add("hidden");
      $("#ideEditorWrap").classList.remove("view-container");
      $("#ideEmpty").classList.remove("hidden");
      ideRenderTabs();
    }
  } else {
    ideRenderTabs();
  }
}

function ideTreeRender() {
  const box = $("#ideTree");
  if (!box) return;
  const paths = Object.keys(state.manifest || {}).filter((p) => p !== "/manifest.json");
  $("#ideTreeMeta").textContent = paths.length ? `${paths.length} 個檔` : "—";
  if (!paths.length) {
    box.innerHTML = '<p class="muted" style="padding:12px">未建立索引：連線後按 ⟳。</p>';
    return;
  }
  const root = { dirs: new Map(), files: [] };
  for (const path of paths) {
    if (!path.startsWith("/")) continue;
    const segs = path.slice(1).split("/").filter(Boolean);
    if (!segs.length) continue;
    let node = root;
    for (let i = 0; i < segs.length - 1; i++) {
      if (!node.dirs.has(segs[i])) node.dirs.set(segs[i], { dirs: new Map(), files: [] });
      node = node.dirs.get(segs[i]);
    }
    node.files.push({ name: segs[segs.length - 1], path, size: Number(state.manifest[path]?.s || 0) });
  }
  box.innerHTML = "";
  const makeFile = (f) => {
    const el = document.createElement("div");
    el.className = "ide-node";
    el.dataset.path = f.path;
    const ex = ideExtInfo(f.path);
    const ic = document.createElement("span");
    ic.className = `ico ext ${ex.cls}`;
    ic.textContent = ex.label;
    el.appendChild(ic);
    const n = document.createElement("span");
    n.textContent = f.name;
    el.appendChild(n);
    const s = document.createElement("span");
    s.className = "sz";
    s.textContent = fmtBytes(f.size);
    el.appendChild(s);
    el.onclick = () => ideOpen(f.path);
    return el;
  };
  const makeDir = (name, node, prefix) => {
    const wrap = document.createElement("div");
    const head = document.createElement("div");
    head.className = "ide-node";
    head.innerHTML = '<span class="ico">📁</span>';
    const n = document.createElement("span");
    n.textContent = name;
    head.appendChild(n);
    const kids = document.createElement("div");
    kids.className = "ide-folder-children";
    const fill = () => {
      if (kids.childElementCount) return;
      for (const [dn, dnode] of [...node.dirs.entries()].sort()) kids.appendChild(makeDir(dn, dnode, prefix + "/" + dn));
      for (const f of node.files.sort((a, b) => a.name.localeCompare(b.name))) kids.appendChild(makeFile(f));
    };
    fill();
    head.onclick = () => {
      kids.classList.toggle("hidden");
      head.querySelector(".ico").textContent = kids.classList.contains("hidden") ? "📁" : "📂";
    };
    wrap.appendChild(head);
    wrap.appendChild(kids);
    return wrap;
  };
  for (const [dn, dnode] of [...root.dirs.entries()].sort()) box.appendChild(makeDir(dn, dnode, "/" + dn));
  for (const f of root.files.sort((a, b) => a.name.localeCompare(b.name))) box.appendChild(makeFile(f));
  ideTreeActive();
}

function ideLintAnnotations(cm) {
  const path = state.ideActive || "";
  if (!/\.json$/i.test(path) || !cm) return [];
  const text = cm.getValue();
  try {
    JSON.parse(text);
    return [];
  } catch (e) {
    const msg = String(e.message || e);
    const m = msg.match(/position (\d+)/i);
    let idx = m ? Number(m[1]) : 0;
    idx = Math.max(0, Math.min(idx, text.length));
    const before = text.slice(0, idx);
    const line = before.split("\n").length - 1;
    const col = idx - (before.lastIndexOf("\n") + 1);
    return [{ from: CodeMirror.Pos(line, col), to: CodeMirror.Pos(line, col + 1),
              message: msg, severity: "error" }];
  }
}

function ideLintStatus(showToast = false) {
  const chip = $("#ideLint");
  const tab = ideActiveTab();
  if (!chip) return;
  if (!tab || !/\.json$/i.test(tab.path)) {
    chip.classList.add("hidden");
    return;
  }
  const errs = ideLintAnnotations(state.ideCM);
  chip.classList.remove("hidden");
  if (errs.length) {
    chip.textContent = "JSON ✗";
    chip.classList.add("err");
    chip.classList.remove("ok");
    if (showToast) toast(errs[0].message, "err", 7000);
  } else {
    chip.textContent = "JSON ✓";
    chip.classList.remove("err");
    chip.classList.add("ok");
    if (showToast) toast("JSON 語法正確", "ok", 3000);
  }
}

function ideApplyTheme(name) {
  const shell = document.querySelector(".ide-shell");
  if (!shell) return;
  shell.dataset.theme = name;
  if (state.ideCM) {
    state.ideCM.setOption("theme", name === "bench" ? "default" : name);
    setTimeout(() => state.ideCM.refresh(), 0);
  }
  const sel = $("#ideTheme");
  if (sel) sel.value = name;
  try { localStorage.setItem("espwm.ideTheme", name); } catch (_) { /* */ }
}

async function ideFormat() {
  const tab = ideActiveTab();
  const cm = state.ideCM;
  if (!tab || !cm) return;
  if (!/\.json$/i.test(tab.path)) {
    toast("格式化目前支援 JSON（其他語言交畀 linter／編輯器）", "warn", 5000);
    return;
  }
  const src = cm.getValue();
  // server 係唯一真相（webmaster/formatting.py）；失敗就唔改內容，逐條報錯。
  try {
    const r = await fetch("/api/format", {
      method: "POST",
      headers: { "Content-Type": "application/json", "X-Actor": "ui" },
      body: JSON.stringify({ path: tab.path, text: src }),
    });
    const j = await r.json();
    if (!j.ok) {
      const d = j.detail || {};
      const at = d.line ? `（第 ${d.line} 行，第 ${d.col || 1} 欄）` : "";
      toast(`JSON 無效，唔會格式化${at}：${d.msg || "唔知咩事"}`, "err", 8000);
      return;
    }
    const out = j.data.text;
    if (!j.data.changed) {
      toast("已經係標準格式", "ok", 2500);
      return;
    }
    cm.setValue(out);
    tab.dirty = true;
    ideRenderTabs();
    ideSyncBar();
    ideLintStatus();
    toast(`已格式化 JSON（${fmtBytes(j.data.bytes_in)} → ${fmtBytes(j.data.bytes_out)}）`, "ok", 2800);
  } catch (e) {
    toast(`格式化失敗：${e.message}`, "err", 7000);
  }
}

async function ideSave() {
  const tab = ideActiveTab();
  const cm = state.ideCM;
  if (!tab || !cm || tab.binary) return;
  const data = new TextEncoder().encode(cm.getValue());
  try {
    $("#ideSize").textContent = "儲存中…";
    await putRaw(tab.path, data);
    tab.text = cm.getValue();
    tab.dirty = false;
    ideRenderTabs();
    toast(`已儲存 ${tab.path}`, "ok");
    logLine(`IDE 儲存 ${tab.path}（${fmtBytes(data.length)}，已自動確認）`, "ok");
    await loadManifest({ quiet: true });
  } catch (e) {
    toast(`儲存失敗：${e.message}`, "err", 7000);
    logLine(`IDE 儲存失敗：${e.message}`, "err");
  } finally {
    ideSyncBar();
  }
}

async function ideRefreshPath(path) {
  const tab = state.ideTabs.find((t) => t.path === path);
  if (!tab || tab.dirty) return;
  try {
    const r = await fetch(`/api/file?path=${encodeURIComponent(path)}`);
    if (!r.ok) return;
    const text = decodeText(new Uint8Array(await r.arrayBuffer()));
    if (text === null) return;
    tab.text = text;
    if (path === state.ideActive && state.ideCM) {
      state.ideSilent = true;
      state.ideCM.setValue(text);
      state.ideSilent = false;
      ideSyncBar();
    }
    ideRenderTabs();
  } catch (_) { /* 下次再算 */ }
}

async function ideReload() {
  const tab = ideActiveTab();
  if (!tab) return;
  try {
    const r = await fetch(`/api/file?path=${encodeURIComponent(tab.path)}`);
    if (!r.ok) throw new Error(`${r.status}`);
    const text = decodeText(new Uint8Array(await r.arrayBuffer()));
    if (text === null) { toast("二進位檔", "warn"); return; }
    tab.text = text;
    tab.dirty = false;
    state.ideSilent = true;
    state.ideCM.setValue(text);
    state.ideSilent = false;
    ideRenderTabs();
    toast("已重新載入", "ok");
  } catch (e) {
    toast(`重新載入失敗：${e.message}`, "err", 6000);
  }
}

async function ideFileOp(op) {
  const tab = ideActiveTab();
  if (!tab) return;
  const label = { confirm: "確認", undo: "復原" }[op] || op;
  try {
    await api(`/api/file/${op}`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: tab.path }),
    });
    toast(`${label}完成：${tab.path}`, "ok");
    logLine(`IDE ${label} ${tab.path}`, "ok");
    await ideReload();
    await loadManifest({ quiet: true });
  } catch (e) {
    toast(`${label}失敗：${e.message}`, "err", 6000);
  }
}

async function ideDelete() {
  const tab = ideActiveTab();
  if (!tab) return;
  if (!confirm(`刪除 ${tab.path}？（裝置上的 .bak 會一併處理）`)) return;
  try {
    await api("/api/file/delete", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ path: tab.path }),
    });
    ideClose(tab.path);
    toast(`已刪除 ${tab.path}`, "ok");
    logLine(`IDE 刪除 ${tab.path}`, "ok");
    await loadManifest({ quiet: true });
  } catch (e) {
    toast(`刪除失敗：${e.message}`, "err", 6000);
  }
}

async function ideNew() {
  const path = prompt("新檔路徑（以 / 開始）", "/config/new.txt");
  if (!path) return;
  try {
    await putRaw(path, new TextEncoder().encode(""));
    await loadManifest({ quiet: true });
    await ideOpen(path);
    toast(`已建立 ${path}`, "ok");
    logLine(`IDE 新檔 ${path}`, "ok");
  } catch (e) {
    toast(`建立失敗：${e.message}`, "err", 6000);
  }
}

function ideDownload() {
  const tab = ideActiveTab();
  if (tab) downloadFile(tab.path);
}

async function ideUpload(files) {
  for (const f of files) {
    try {
      const data = new Uint8Array(await f.arrayBuffer());
      await putRaw("/" + f.name, data);
      logLine(`IDE 上載 ${f.name}（${fmtBytes(data.length)}）`, "ok");
    } catch (e) {
      toast(`上載 ${f.name} 失敗：${e.message}`, "err", 7000);
    }
  }
  await loadManifest({ quiet: true });
}
