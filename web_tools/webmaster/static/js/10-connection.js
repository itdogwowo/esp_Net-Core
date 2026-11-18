/* ESP WebMaster — 連線：序列埠、裝置切換、WebSocket
 * setConnected／refreshPorts／connect／disconnect／openWS／handleEvent。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

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
  $("#btnConnect").disabled = false;   // 可以繼續新增第二台
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
      return;
    }
    // 真板排前：`board` 由 server 判定（link.is_likely_board：VID 白名單＋
    // 排除 Intel AMT／藍牙 SPP），前端唔重複一套 heuristic。
    const sorted = [...ports].sort((a, b) => (b.board ? 1 : 0) - (a.board ? 1 : 0));
    for (const p of sorted) {
      const o = document.createElement("option");
      o.value = p.device;
      const desc = (p.description || "").replace(/\s*\(COM\d+\)\s*$/i, "");
      o.textContent = `${p.board ? "★ " : ""}${p.device} — ${desc || "序列裝置"}`;
      o.title = p.board ? "疑似 ESP 板（USB 序列）" : "系統序列埠（唔似係板）";
      if (p.board) o.dataset.board = "true";
      sel.appendChild(o);
    }
    if (keep) sel.value = keep;
    else if (j.suggested) sel.value = j.suggested;  // 預設揀板，唔好揀到 AMT／藍牙
  } catch (e) {
    console.warn("ports:", e);
  }
}

async function connect(port) {
  if (state.ideTabs.some((t) => t.dirty) &&
      !confirm("有未儲存的 IDE 檔案；新增／切換裝置會關閉它們，繼續？")) {
    return;
  }
  $("#led").dataset.state = "connecting";
  state.switching = true;
  try {
    const j = await api("/api/connect", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(port === "sim" ? { sim: true, activate: true }
                                           : { port, activate: true }),
    });
    logLine(`連線 ${j.port}${j.sim ? "（模擬）" : ""}`, "ok");
    openWS();
    state.device = "";
    await selectDevice(j.device, { force: true });
    state.switching = false;
    switchView("ide");
  } catch (e) {
    state.switching = false;
    $("#led").dataset.state = "error";
    toast(`連線失敗：${e.message}`, "err", 6000);
    logLine(`連線失敗：${e.message}`, "err");
  }
}

async function refreshDevices() {
  try {
    const j = await api("/api/devices");
    const devices = j.devices || [];
    const sel = $("#deviceSelect");
    if (!sel) return;
    if (!devices.length) {
      sel.classList.add("hidden");
      sel.innerHTML = "";
      state.device = "";
      return;
    }
    sel.classList.remove("hidden");
    sel.innerHTML = "";
    for (const d of devices) {
      const o = document.createElement("option");
      o.value = d.device;
      // 真板顯示真實埠（COM20／/dev/cu.usbmodem…）；模擬要一眼睇得出唔係硬件
      const label = d.sim ? "模擬裝置（無硬件）" : String(d.port || d.device);
      o.textContent = `${label}${d.connected ? "" : "（斷）"}`;
      o.title = d.sim
        ? "模擬裝置：虛擬 FAT，所有操作唔會碰到真板。要真板請揀真實序列埠。"
        : `真實序列埠 ${d.port}`;
      if (d.sim) o.dataset.sim = "true";
      sel.appendChild(o);
    }
    if (!state.device) state.device = j.active || "";
    sel.value = state.device || j.active || "";
  } catch (_) { /* 未連線／server 未起 */ }
}

async function selectDevice(key, { force = false } = {}) {
  if (!key) { await refreshDevices(); return; }
  if (!force && key === state.device) { await refreshDevices(); return; }
  if (state.ideTabs.some((t) => t.dirty) &&
      !confirm("切換裝置會關閉未儲存的 IDE 檔案，繼續？")) {
    await refreshDevices();
    return;
  }
  state.switching = true;
  try {
    await api("/api/select", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ device: key }),
    });
  } catch (e) {
    state.switching = false;
    toast(`切換失敗：${e.message}`, "err", 5000);
    return;
  }
  state.device = key;
  state.lastSeq = 0;
  state.session = "";
  state.logs = [];
  state.ideTabs = [];
  state.ideActive = null;
  const term = $("#terminal");
  if (term) term.innerHTML = "";
  const act = $("#activity");
  if (act) act.innerHTML = "";
  if (state.ideCM) {
    state.ideSilent = true;
    state.ideCM.setValue("");
    state.ideSilent = false;
  }
  if ($("#ideEmpty")) $("#ideEmpty").classList.remove("hidden");
  ideRenderTabs();

  try {
    const info = await api("/api/info");
    setConnected(!!info.connected, info.port, info.sim);
    await refreshDevices();
    try {
      const t = await api(`/api/log/tail?n=300&device=${encodeURIComponent(key)}`);
      for (const ev of (t.events || [])) handleEvent(ev);
    } catch (_) { /* */ }
    if (info.connected) {
      try { await refreshStatus(); } catch (_) { /* */ }
      try { await loadManifest({ quiet: true }); } catch (_) { /* */ }
    }
  } finally {
    state.switching = false;
  }
}

async function disconnect() {
  if (state.ideTabs.some((t) => t.dirty) &&
      !confirm("有未儲存的 IDE 檔案；斷線會關閉它們，繼續？")) {
    return;
  }
  let active = null;
  state.switching = true;
  try {
    const j = await api("/api/disconnect", { method: "POST" });
    active = j.active;
  } catch (_) { /* ignore */ }
  await refreshDevices();
  if (active) {
    state.device = "";
    await selectDevice(active, { force: true });
    logLine("已斷線，切換到另一台", "warn");
  } else {
    closeWS();
    setConnected(false);
    state.switching = false;
    logLine("已斷線（冇其他裝置）", "warn");
  }
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
  if (ev.type === "backlog") {
    for (const e of (ev.events || [])) handleEvent(e);
    return;
  }
  if (ev.type === "devices") {
    // devices 事件本身就帶 device=active；唔可以落入下面的 active 過濾。
    refreshDevices().then(async () => {
      if (state.switching || !ev.active || ev.active === state.device) return;
      if (state.ideTabs.some((t) => t.dirty)) {
        toast("另一邊切換了裝置；你有未儲存檔案，已切回原本裝置", "warn", 9000);
        try {
          await api("/api/select", {
            method: "POST", headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ device: state.device }),
          });
        } catch (_) { /* */ }
        await refreshDevices();
      } else {
        await selectDevice(ev.active, { force: true });
      }
    });
    return;
  }
  if (ev.seq != null) {
    if (ev.seq <= state.lastSeq) return;
    state.lastSeq = ev.seq;
  }
  // 多裝置：只顯示目前 active 的事件；背景裝置的事件留在 server ring。
  if (ev.device && state.device && ev.device !== state.device) {
    if (ev.type === "disconnected") refreshDevices();
    return;
  }
  switch (ev.type) {
    case "hello":
      if (ev.device && !state.device) state.device = ev.device;
      if (ev.devices) refreshDevices();
      if (ev.session && ev.session !== state.session) {
        // server 重啟 → seq 由 1 再開始；清掉舊畫面，否則新事件會被 lastSeq 擋掉。
        state.session = ev.session;
        state.lastSeq = 0;
        state.logs = [];
        const term = $("#terminal");
        if (term) term.innerHTML = "";
        const act = $("#activity");
        if (act) act.innerHTML = "";
      }
      if (ev.connected && !state.connected) setConnected(true, ev.port, ev.sim);
      break;
    case "line":
      logLine(ev.text);
      if (ev.flash) {
        const box = $("#espResult");
        if (box) {
          box.classList.remove("hidden");
          box.textContent = (box.textContent === "esptool 啟動中…" ? "" : box.textContent)
            + ev.text + "\n";
          box.scrollTop = box.scrollHeight;
        }
      }
      break;
    case "frame": {
      const n = (ev.len != null) ? ev.len : ((ev.hex || "").length / 2);
      logLine(`«frame ${ev.cmd}  ${n} B»`, "frame");
      break;
    }
    case "activity":
      activityLine(ev);
      break;
    case "file_changed": {
      const tab = state.ideTabs.find((t) => t.path === ev.path);
      if (!tab) break;
      if (ev.deleted) {
        if (!tab.dirty) ideClose(tab.path);
        else toast(`${ev.actor || "?"} 刪除了 ${ev.path}（你這邊有未儲存修改）`, "warn", 8000);
      } else if (tab.dirty) {
        // 自己儲存時 server 也會廣播 file_changed（actor=ui）；唔好對自己彈警告。
        if (ev.actor && ev.actor !== "ui") {
          toast(`${ev.actor} 改了 ${ev.path}；你這邊有未儲存修改，儲存會覆蓋`, "warn", 8000);
        }
      } else {
        ideRefreshPath(tab.path);
      }
      break;
    }
    case "flash_request":
      // AI 發起頁面燒錄：彈出確認（人等按一下，Web Serial 需要 user gesture）
      if (typeof bfOnRequest === "function") bfOnRequest(ev);
      break;
    case "flash_result":
      if (typeof bfOnResult === "function") bfOnResult(ev);
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
      if (ev.op === "esptool") {
        setProgress("#espProgress", "#espProgressWrap", ev.done, ev.total);
      }
      break;
    case "flash":
      if (ev.phase === "start") {
        logLine(`ESPTOOL 開始（${ev.bytes || 0} B，port ${ev.port || "?"}）`, "warn");
        toast("ESPTOOL 燒錄中…", "warn", 5000);
      } else if (ev.phase === "done") {
        logLine(`ESPTOOL 完成（rc ${ev.rc}）`, ev.rc === 0 ? "ok" : "err");
        setTimeout(() => $("#espProgressWrap").classList.add("hidden"), 1200);
      }
      break;
    case "disconnected":
      setConnected(false);
      refreshDevices();
      logLine(`連線中斷：${ev.reason || "unknown"}`, "err");
      break;
    default:
      break;
  }
}
