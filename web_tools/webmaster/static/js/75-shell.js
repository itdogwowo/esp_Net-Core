/* ESP WebMaster — 外殼：頁面切換、事件綁定
 * switchView（rail 導航）＋ bindUI（所有按鈕）。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

// ── 版面 ───────────────────────────────────────────────
function switchView(name) {
  if (name === "files") name = "ide";   // 舊「檔案」頁已併入 IDE
  state.view = name;
  if (location.hash !== `#${name}`) history.replaceState(null, "", `#${name}`);
  $$(".rail .nav[data-view]").forEach((b) =>
    b.setAttribute("aria-current", b.dataset.view === name ? "true" : "false"));
  $$(".view").forEach((v) => v.dataset.on = v.dataset.view === name ? "true" : "false");
  if (name === "files" && state.connected) {
    if (!Object.keys(state.manifest || {}).length) loadManifest();
    else { renderCrumb(state.cwd); renderFiles(entriesFor(state.cwd)); }
  }
  if (name === "drive" && state.driveRoot) loadDrive(state.drivePath);
  if (name === "flash") bfRefresh();
  if (name === "env") envLoad();
  if (name === "ide") {
    const cm = ideEnsureCM();
    if (state.connected && !Object.keys(state.manifest || {}).length) {
      loadManifest().then(() => ideTreeRender()).catch(() => {});
    } else {
      ideTreeRender();
    }
    if (cm) setTimeout(() => cm.refresh(), 0);
  }
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
  $("#deviceSelect").onchange = (e) => selectDevice(e.target.value);
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

  // IDE
  $("#btnIdeSave").onclick = ideSave;
  $("#btnIdeConfirm").onclick = () => ideFileOp("confirm");
  $("#btnIdeUndo").onclick = () => ideFileOp("undo");
  $("#btnIdeDownload").onclick = ideDownload;
  $("#btnIdeDelete").onclick = ideDelete;
  $("#btnIdeFormat").onclick = ideFormat;
  $("#btnIdeValidate").onclick = () => ideLintStatus(true);
  $("#ideTheme").onchange = (e) => ideApplyTheme(e.target.value);
  $("#btnIdeScan").onclick = () => rescan().then(() => ideTreeRender());
  $("#btnIdeNew").onclick = ideNew;
  $("#btnIdeUpload").onclick = () => $("#ideFilePicker").click();
  $("#ideFilePicker").onchange = (e) => {
    ideUpload(e.target.files);
    e.target.value = "";
  };
  const ideShell = document.querySelector(".ide-shell");
  if (ideShell) {
    ideShell.addEventListener("dragover", (e) => { e.preventDefault(); });
    ideShell.addEventListener("drop", (e) => {
      e.preventDefault();
      if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length) {
        ideUpload(e.dataTransfer.files);
      }
    });
  }
  $("#tabTerm").onclick = () => showPanelTab("term");
  $("#tabActivity").onclick = () => showPanelTab("activity");

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
  $("#btnEspPick").onclick = () => $("#espPicker").click();
  $("#espPicker").onchange = (e) => pickEspFile(e.target.files[0]);
  $("#btnEspFlash").onclick = startEsptool;
  $("#btnEspInfo").onclick = async () => {
    if (state.sim) { toast("模擬模式唔可以探測", "warn"); return; }
    $("#btnEspInfo").disabled = true;
    $("#espResult").classList.remove("hidden");
    $("#espResult").textContent = "探測中（會短暫放開序列埠）…";
    try {
      const q = new URLSearchParams({
        chip: $("#espChip").value || "",
        before: $("#espBefore").value || "usb_reset",
      });
      const j = await api(`/api/flash/info?${q}`);
      $("#espResult").textContent = (j.lines || []).join("\n") || "（冇輸出）";
      toast(j.ok ? "晶片探測完成" : `探測失敗 rc=${j.rc}`,
            j.ok ? "ok" : "err", 5000);
      try {
        const info = await api("/api/info");
        setConnected(!!info.connected, info.port, info.sim);
        if (info.connected) { openWS(); await refreshStatus(); }
      } catch (_) { /* */ }
    } catch (e) {
      $("#espResult").textContent = `失敗：${e.message}`;
      toast(`探測失敗：${e.message}`, "err", 7000);
    } finally {
      $("#btnEspInfo").disabled = false;
    }
  };

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

  // 快捷鍵：Ctrl/Cmd-1..6 切 view；IDE 內 Ctrl/Cmd-S 儲存
  window.addEventListener("keydown", (e) => {
    if (!(e.ctrlKey || e.metaKey)) return;
    if (e.key.toLowerCase() === "s" && state.view === "ide") {
      e.preventDefault();
      ideSave();
      return;
    }
    if (e.shiftKey && e.key.toLowerCase() === "f" && state.view === "ide") {
      e.preventDefault();
      ideFormat();
      return;
    }
    const map = { "1": "device", "2": "ide", "3": "drive", "4": "flash",
                  "5": "monitor", "6": "env" };
    if (map[e.key]) { e.preventDefault(); switchView(map[e.key]); }
  });

  window.addEventListener("beforeunload", (e) => {
    if (state.dirty || state.driveDirty || state.ideTabs.some((t) => t.dirty)) {
      e.preventDefault();
      e.returnValue = "";
    }
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
