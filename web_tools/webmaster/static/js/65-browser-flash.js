/* ESP WebMaster — 頁面燒錄（esptool-js／Web Serial）
 * 第三條燒錄路徑；序列埠要同 server 交接（release → flash → reclaim）。
 */
"use strict";

// ═══════════════════════════════════════════════════════════════════════
// 頁面燒錄（esptool-js／Web Serial）—— 第三條燒錄路徑
//
// 三條路的分工：
//   1. NC4 OTA（POST /api/firmware）      S3／C3 日常更新，唔使放開序列埠
//   2. Python esptool（/api/flash/esptool）P4、救磚、第一次燒 bootloader
//   3. 本檔（esptool-js＋Web Serial）      零安裝、純瀏覽器；要交出／交還序列埠
//
// 為什麼要「交接」：COM 埠同一時間只有一個主人。server 揸住時，瀏覽器
// navigator.serial.requestPort() 一定開唔到（PermissionError）。所以順序必須係
//   POST /api/port/release → requestPort() → 燒 → POST /api/port/reclaim
// server 在交接期間**唔會**自己重開埠（見 server.py 的 State.handover）。
// ═══════════════════════════════════════════════════════════════════════

function bfLog(text, cls = "") {
  const box = $("#bfLog");
  if (!box) return;
  const line = document.createElement("div");
  line.className = "line " + cls;
  line.textContent = text;
  box.appendChild(line);
  while (box.childElementCount > 400) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}

function bfSetState(text, kind = "") {
  const chip = $("#bfState");
  if (!chip) return;
  chip.textContent = text;
  chip.className = "chip" + (kind ? " " + kind : "");
}

function bfHasWebSerial() {
  return typeof navigator !== "undefined" && "serial" in navigator;
}

function bfHasLib() {
  return !!(window.esptooljs && window.esptooljs.ESPLoader && window.esptooljs.Transport);
}

/** 重新讀 server 側的交接狀態，同步按鈕。 */
async function bfRefresh() {
  let st;
  try {
    st = await api("/api/port/state");
  } catch (e) {
    bfSetState("讀唔到狀態", "");
    return;
  }
  const libOk = bfHasLib();
  const wsOk = bfHasWebSerial();
  $("#btnBfRelease").disabled = !(st.handover === false && state.connected);
  $("#btnBfPick").disabled = !(st.handover === true && libOk && wsOk);
  $("#btnBfReclaim").disabled = st.handover !== true;

  if (!wsOk) bfSetState("瀏覽器唔支援 Web Serial", "amber");
  else if (!libOk) bfSetState("esptool-js 未載入", "amber");
  else if (st.handover) {
    bfSetState(`已交給瀏覽器${st.port ? "：" + st.port : ""}` + (st.stale ? "（可能已失效）" : ""), "amber");
  } else bfSetState("server 揸住序列埠", "ok");

  if (!st.browserFlash) {
    bfSetState("設定已停用", "amber");
    $("#btnBfRelease").disabled = $("#btnBfPick").disabled = $("#btnBfReclaim").disabled = true;
  }
  return st;
}

/** 1. 交出序列埠 */
async function bfRelease() {
  try {
    const j = await api("/api/port/release", { method: "POST" });
    bfLog(`已交出 ${j.port}（server 已關閉序列埠）`, "ok");
    toast(`已交出 ${j.port}，可以揀埠燒錄`, "ok");
    setConnected(false);
  } catch (e) {
    toast(`交出失敗：${e.message}`, "err", 7000);
    bfLog(`交出失敗：${e.message}`, "err");
  }
  await bfRefresh();
}

/** 3. 交還序列埠（server 重新連線） */
async function bfReclaim() {
  try {
    const j = await api("/api/port/reclaim", { method: "POST" });
    bfLog(`已交還，server 重新連到 ${j.port}`, "ok");
    toast(`已交還：${j.port}`, "ok");
    setConnected(true, j.port, false);
    openWS();
    await refreshStatus();
  } catch (e) {
    bfLog(`交還失敗：${e.message}`, "err");
    toast(`交還失敗：${e.message}`, "err", 9000);
  }
  await bfRefresh();
}

function bfSetProgress(done, total) {
  const wrap = $("#bfProgressWrap");
  const bar = $("#bfProgress");
  if (!wrap || !bar) return;
  if (done == null) { wrap.classList.add("hidden"); return; }
  wrap.classList.remove("hidden");
  const pct = total ? Math.min(100, Math.round((done / total) * 100)) : 0;
  bar.style.width = pct + "%";
}

/** 2. 揀埠 → 燒錄。全程用 esptool-js。 */
async function bfFlash() {
  if (!bfHasLib()) { toast("esptool-js 未載入（檢查 /static/vendor/esptool-js/loader.js）", "err", 8000); return; }
  if (!bfHasWebSerial()) {
    toast("呢個瀏覽器唔支援 Web Serial（要 Chrome／Edge 89+）", "err", 9000);
    return;
  }
  const btn = $("#btnBfPick");
  btn.disabled = true;

  let port = null;
  let transport = null;
  try {
    // (a) 揀埠：必須 user gesture（呢個 function 由 click 直接呼叫，OK）
    bfLog("叫出瀏覽器選埠對話框…");
    // 只列出似 ESP 的裝置（同 server 側同一組 VID；瀏覽器亦支援 filters）
    port = await navigator.serial.requestPort({
      filters: [
        { usbVendorId: 0x303a },   // Espressif
        { usbVendorId: 0x10c4 },   // CP210x
        { usbVendorId: 0x1a86 },   // CH34x
        { usbVendorId: 0x0403 },   // FTDI
      ],
    });
    const label = (port.getInfo && port.getInfo().usbVendorId != null)
      ? `VID 0x${port.getInfo().usbVendorId.toString(16)}` : "已選埠";
    bfLog(`已揀埠（${label}）`);

    const { ESPLoader, Transport } = window.esptooljs;
    transport = new Transport(port, true);
    const terminal = {
      clean() { /* 終端清空：我哋自己嘅 bfLog 唔清 */ },
      writeLine(d) { bfLog(String(d).trim()); },
      write(d) { bfLog(String(d).trim()); },
    };
    const loader = new ESPLoader({
      transport, baudrate: parseInt($("#bfBaud").value, 10) || 460800,
      terminal, romBaudrate: 115200,
    });

    bfLog("連線中（會 reset 板子）…");
    const chip = await loader.main();
    bfLog(`偵測到晶片：${chip}`, "ok");

    // (b) 讀 .bin：用同一個 file picker（唔需要另開卡片）
    const file = await bfPickBin();
    if (!file) { bfLog("冇揀 .bin，取消"); bfSetProgress(null); return; }
    const data = new Uint8Array(await file.arrayBuffer());
    if (!data.length) throw new Error("檔案係空");
    bfLog(`檔案：${file.name}（${fmtBytes(data.length)}）`);

    // (c) 燒
    const addr = parseInt($("#bfOffset").value, 10) || 0;
    const opts = {
      fileArray: [{ data, address: addr }],
      flashMode: $("#bfMode").value,
      flashFreq: $("#bfFreq").value,
      flashSize: $("#bfSize").value,
      eraseAll: $("#bfErase").checked,
      compress: true,
      reportProgress: (_i, written, total) => {
        bfSetProgress(written, total);
        if (written === total || written % (64 * 1024) < 4096) {
          bfLog(`寫入 ${fmtBytes(written)} / ${fmtBytes(total)}`);
        }
      },
    };
    bfLog(`開始燒錄 @ ${"0x" + addr.toString(16)} …`);
    await loader.writeFlash(opts);
    bfLog("燒錄完成，reset 板子…", "ok");
    bfSetProgress(1, 1);

    await loader.after("hard_reset");
    bfLog("已 reset。", "ok");
    toast("頁面燒錄完成", "ok", 4000);
  } catch (e) {
    const msg = e && e.message ? e.message : String(e);
    bfLog(`失敗：${msg}`, "err");
    toast(`頁面燒錄失敗：${msg}`, "err", 12000);
  } finally {
    // 一定要釋放，否則 server 交還時會 PermissionError
    try { if (transport && transport.disconnect) await transport.disconnect(); } catch (_) { /* */ }
    try { if (port && port.close) await port.close(); } catch (_) { /* */ }
    await new Promise((r) => setTimeout(r, 400));
    bfSetProgress(null);
    await bfRefresh();
    if (!state.connected) {
      bfLog("提示：按「3 · 交還序列埠」讓 server 重新連線。", "warn");
    }
  }
}

/** 用一次性的 <input type=file> 揀 .bin（回 Promise<File|null>）。 */
function bfPickBin() {
  return new Promise((resolve) => {
    const inp = document.createElement("input");
    inp.type = "file";
    inp.accept = ".bin";
    inp.onchange = () => resolve(inp.files && inp.files[0] ? inp.files[0] : null);
    // 用戶按取消時 change 唔會 fire；用 focus 回來做 fallback
    window.addEventListener("focus", () => {
      setTimeout(() => { if (!inp.files || !inp.files.length) resolve(null); }, 700);
    }, { once: true });
    inp.click();
  });
}

/** 自檢：把「呢個瀏覽器／呢個 server 能唔能頁面燒錄」逐項講清楚。 */
function bfHelp() {
  const rows = [
    ["Web Serial（navigator.serial）", bfHasWebSerial() ? "有" : "冇（要 Chrome／Edge 89+）"],
    ["esptool-js 已載入", bfHasLib() ? "有" : "冇（loader.js 或 bundle.js 未載入）"],
    ["安全上下文（https 或 localhost）", window.isSecureContext ? "是" : "否（Web Serial 需要）"],
    ["瀏覽器", navigator.userAgent.slice(0, 90)],
  ];
  bfLog("── 自檢 ──");
  for (const [k, v] of rows) bfLog(`${k}：${v}`, v === "冇" ? "err" : "");
  toast("自檢結果寫在下面日誌", "ok", 3000);
}

function bindBrowserFlash() {
  if (!$("#btnBfRelease")) return;
  $("#btnBfRelease").onclick = bfRelease;
  $("#btnBfReclaim").onclick = bfReclaim;
  $("#btnBfPick").onclick = bfFlash;
  $("#btnBfHelp").onclick = bfHelp;
  window.addEventListener("esptooljs-ready", () => { bfRefresh(); });
  // 進固件頁 / 連線狀態變 → 刷新一次（唔用 poll，避免無謂請求）
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden && state.view === "flash") bfRefresh();
  });
}
