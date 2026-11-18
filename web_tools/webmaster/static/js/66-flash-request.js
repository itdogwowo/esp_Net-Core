
// ═══════════════════════════════════════════════════════════════════════
// 頁面燒錄：AI 請求 → 人按一下 → 瀏覽器燒 → 結果回 AI
//
// 為什麼要「人按一下」：Web Serial 的 navigator.serial.requestPort() 必須由
// user gesture 觸發（瀏覽器安全模型）。所以 AI 不能直接燒，只能：
//   POST /api/flash/request（上傳 .bin）
//     → UI 收到 WS `flash_request`，彈出這個橫幅
//     → 人按「執行」→ 這裡自動做 release → requestPort → writeFlash → reclaim
//     → POST /api/flash/request/{id}/result
// AI 之後用 GET /api/flash/request 讀結果（done／failed＋error）。
// ═══════════════════════════════════════════════════════════════════════
"use strict";

let bfPendingReq = null;

function bfReqShow(req) {
  bfPendingReq = req;
  const box = $("#bfReqBox");
  if (!box) return;
  if (!req || req.status === "done" || req.status === "failed" || req.status === "cancelled") {
    box.classList.add("hidden");
    return;
  }
  box.classList.remove("hidden");
  $("#bfReqInfo").textContent =
    `來源：${req.by || "api"} ｜ 檔案：${fmtBytes(req.size)} ｜ offset 0x${(req.offset || 0).toString(16)}`
    + ` ｜ mode ${req.mode}／freq ${req.freq}／size ${req.sizeOpt}`
    + (req.eraseAll ? " ｜ 整片抹除" : "")
    + (req.note ? `\n備註：${req.note}` : "");
  $("#bfReqInfo").style.whiteSpace = "pre-wrap";
  // 橫幅一出現就提示（AI 的操作不應靜默）
  toast(`AI 請求燒錄（${fmtBytes(req.size)}）：去固件頁按「執行」`, "warn", 12000);
}

/** WS `flash_request` */
function bfOnRequest(ev) {
  bfReqShow(ev);
  if (state.view !== "flash") {
    logLine(`[ai] 請求燒錄 ${fmtBytes(ev.size)} @ 0x${(ev.offset || 0).toString(16)} — 固件頁按「執行」`, "warn");
  }
}

/** WS `flash_result` */
function bfOnResult(ev) {
  if (!bfPendingReq || ev.id === bfPendingReq.id) bfReqShow(null);
  if (ev.status === "done") {
    toast("AI 請求的頁面燒錄已完成", "ok", 5000);
    bfLog(`AI 請求 ${ev.id}：完成`, "ok");
  } else if (ev.status === "failed") {
    toast(`AI 請求的頁面燒錄失敗：${ev.error || ""}`, "err", 9000);
    bfLog(`AI 請求 ${ev.id}：失敗 ${ev.error || ""}`, "err");
  }
}

/** 按「執行」：claim → 交接 → 選埠 → 燒 → 交還 → 回報 */
async function bfRunRequest() {
  const req = bfPendingReq;
  if (!req) return;
  const run = $("#btnBfReqRun");
  run.disabled = true;
  let holdPort = false;
  try {
    // 1) 認領（避免兩個分頁同時燒）
    await api(`/api/flash/request/${req.id}/claim`, { method: "POST" });
    bfLog(`接手 AI 請求 ${req.id}（${fmtBytes(req.size)}）`);

    // 2) 交出序列埠（如果 server 揸住）
    const st = await api("/api/port/state");
    if (!st.handover && state.connected) {
      await api("/api/port/release", { method: "POST" });
      holdPort = true;
      bfLog("已交出序列埠");
      setConnected(false);
    }

    // 3) 拿映像 bytes
    const r = await fetch(`/api/flash/request/${req.id}/image`);
    if (!r.ok) throw new Error(`下載映像失敗：${r.status}`);
    const data = new Uint8Array(await r.arrayBuffer());

    // 4) 揀埠（真正的 user gesture 是在這個 click handler 內）
    const port = await navigator.serial.requestPort({
      filters: [{ usbVendorId: 0x303a }, { usbVendorId: 0x10c4 },
                { usbVendorId: 0x1a86 }, { usbVendorId: 0x0403 }],
    });
    const { ESPLoader, Transport } = window.esptooljs;
    const transport = new Transport(port, true);
    const loader = new ESPLoader({
      transport, baudrate: parseInt($("#bfBaud").value, 10) || 460800,
      terminal: { clean() {}, writeLine(d) { bfLog(String(d).trim()); },
                  write(d) { bfLog(String(d).trim()); } },
      romBaudrate: 115200,
    });
    bfLog("連線中…");
    const chip = await loader.main();
    bfLog(`晶片：${chip}`, "ok");
    bfLog(`燒錄 @ 0x${req.offset.toString(16)} …`);
    await loader.writeFlash({
      fileArray: [{ data, address: req.offset }],
      flashMode: req.mode, flashFreq: req.freq, flashSize: req.sizeOpt,
      eraseAll: !!req.eraseAll, compress: true,
      reportProgress: (_i, w, t) => bfSetProgress(w, t),
    });
    await loader.after("hard_reset");
    try { await transport.disconnect(); } catch (_) { /* */ }
    try { await port.close(); } catch (_) { /* */ }
    bfSetProgress(1, 1);

    // 5) 交還
    if (holdPort) { await api("/api/port/reclaim", { method: "POST" }); }
    // 6) 回報結果（AI 之後讀得到）
    await api(`/api/flash/request/${req.id}/result`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ ok: true }),
    });
    bfLog(`AI 請求 ${req.id}：完成`, "ok");
    bfReqShow(null);
  } catch (e) {
    const msg = (e && e.message) ? e.message : String(e);
    bfLog(`AI 請求失敗：${msg}`, "err");
    try {
      await api(`/api/flash/request/${req.id}/result`, {
        method: "POST", headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ ok: false, error: msg }),
      });
    } catch (_) { /* 回報失敗都唔阻 */ }
    if (holdPort && !state.connected) {
      try { await api("/api/port/reclaim", { method: "POST" }); } catch (_) { /* */ }
    }
    bfReqShow(null);
  } finally {
    run.disabled = false;
    bfSetProgress(null);
    await bfRefresh();
  }
}

async function bfCancelRequest() {
  const req = bfPendingReq;
  if (!req) return;
  try {
    await api(`/api/flash/request/${req.id}/cancel`, { method: "POST" });
    bfLog(`已拒絕 AI 請求 ${req.id}`);
  } catch (e) {
    toast(`拒絕失敗：${e.message}`, "err", 6000);
  }
  bfReqShow(null);
}

function bindFlashRequest() {
  if (!$("#btnBfReqRun")) return;
  $("#btnBfReqRun").onclick = bfRunRequest;
  $("#btnBfReqCancel").onclick = bfCancelRequest;
  // 開頁時檢查有冇待處理請求（例如 AI 發請求時 UI 未開）
  api("/api/flash/request").then((j) => {
    const req = j.data;
    if (req && (req.status === "pending" || req.status === "claimed")) bfReqShow(req);
  }).catch(() => { /* 未連 server 就算 */ });
}
