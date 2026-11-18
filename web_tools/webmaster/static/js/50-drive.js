/* ESP WebMaster — 隨身碟頁（MSC 捷徑）
 * 本機資料夾 ↔ 板子 FAT；只有在板子會枚舉成 USB drive 時有用。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

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
