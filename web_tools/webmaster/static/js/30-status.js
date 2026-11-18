/* ESP WebMaster — 設備頁：STATUS 讀取
 * refreshStatus；sys／storage／log 三張卡。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

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
