/* ESP WebMaster — 啟動：init、deep link、鍵盤快捷鍵
 * DOMContentLoaded 之後跑；最後一個載入。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

// ── 啟動 ───────────────────────────────────────────────
async function init() {
  bindUI();
  bindBrowserFlash();
  bindEnv();
  bindFlashRequest();
  // 伺服器設定（settings.json）：ui.default_view 等；失敗唔應該阻住開機
  let defaultView = "device";
  try {
    const cfgResp = await api("/api/config");
    if (cfgResp.ok) {
      state.serverCfg = cfgResp.effective || {};
      const dv = state.serverCfg.ui && state.serverCfg.ui.default_view;
      if (dv) defaultView = dv === "files" ? "ide" : dv;
    }
  } catch (_) { /* 設定讀唔到就照內建預設 */ }

  const deep = location.hash.replace("#", "");
  const deepOk = ["device", "files", "drive", "flash", "monitor", "ide", "env", "wave", "mode"].includes(deep);
  if (deepOk) {
    switchView(deep);
  } else {
    // 冇 deep link：用設定檔的 ui.default_view（預設「啟動」＝入門頁）；
    // 連上裝置之後會自動跳去「設備」。
    switchView(defaultView);
  }
  loadCommands();
  refreshPorts();
  loadDrives();
  setInterval(() => { if (!state.connected) refreshPorts(); }, 4000);

  logLine("ESP WebMaster 已啟動。揀 port → 連線，或按『模擬裝置』。", "info");
  const qs = new URLSearchParams(location.search);
  const deepFile = qs.get("file");
  if (qs.get("panel") === "activity") showPanelTab("activity");
  try {
    const j = await api("/api/info");
    if (j.device) state.device = j.device;
    // 設定叫你連但連唔到（例：埠被另一個程式佔用）→ 一定要出聲，
    // 否則用戶見到「未連線」會以為板壞。
    if (j.autoconnectError) {
      logLine(`⚠ ${j.autoconnectError}`, "err");
      toast(j.autoconnectError, "err", 12000);
    }
    await refreshDevices();
    if (j.connected) {
      setConnected(true, j.port, j.sim);
      openWS();
      await refreshStatus();
      // 有裝置但用戶冇指定頁：由入門頁跳去「設備」，唔會打斷深層連結
      if (!deepOk && !qs.get("file")) switchView("device");
      if (!(await loadManifest({ quiet: true }))) await rescan();
      if (deepFile) {
        switchView("ide");
        await ideOpen(deepFile);
      }
    }
  } catch (e) {
    logLine(`伺服器狀態讀取失敗：${e.message}`, "err");
  }
}

document.addEventListener("DOMContentLoaded", init);
