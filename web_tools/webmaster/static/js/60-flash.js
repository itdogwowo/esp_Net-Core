/* ESP WebMaster — 固件頁：NC4 OTA 與 ESPTOOL
 * OTA（0x22xx）同 esptool 燒錄（server 側放開序列埠再燒）。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

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
  xhr.setRequestHeader("X-Actor", "ui");
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


// ── ESPTOOL（ROM／USJ）────────────────────────────────
function pickEspFile(file) {
  if (!file) return;
  state.espFile = file;
  $("#espFileInfo").textContent = `${file.name} · ${fmtBytes(file.size)}`;
  $("#btnEspFlash").disabled = false;
  $("#espErase").checked = false;
}

function startEsptool() {
  if (state.sim) { toast("模擬模式唔可以 esptool 燒錄", "warn"); return; }
  const erase = $("#espErase").checked;
  if (!erase && !state.espFile) { toast("先揀 .bin（或勾選 erase）", "warn"); return; }
  if (!confirm(erase
    ? "只 erase flash？（所有資料／FAT 會清空）"
    : `ESPTOOL 燒錄 ${state.espFile.name} @ ${$("#espOffset").value}？\n燒錄期間會斷線，完成後自動重連。`)) return;

  const q = new URLSearchParams({
    offset: $("#espOffset").value || "0x0",
    baud: $("#espBaud").value || "460800",
    chip: $("#espChip").value || "",
    before: $("#espBefore").value || "usb_reset",
    erase: erase ? "1" : "0",
  });
  const xhr = new XMLHttpRequest();
  xhr.open("POST", `/api/flash/esptool?${q}`);
  xhr.setRequestHeader("X-Actor", "ui");
  xhr.responseType = "json";
  $("#btnEspFlash").disabled = true;
  $("#led").dataset.state = "busy";
  $("#espResult").classList.remove("hidden");
  $("#espResult").textContent = "esptool 啟動中…";
  setProgress("#espProgress", "#espProgressWrap", 0, 100);
  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) setProgress("#espProgress", "#espProgressWrap", e.loaded, e.total);
  };
  xhr.onload = async () => {
    $("#btnEspFlash").disabled = false;
    let msg = xhr.response?.detail ?? xhr.statusText;
    if (xhr.status >= 200 && xhr.status < 300) {
      $("#espResult").textContent = "ESPTOOL 完成";
      toast("ESPTOOL 完成", "ok", 5000);
      try {
        const info = await api("/api/info");
        setConnected(!!info.connected, info.port, info.sim);
        if (info.connected) { openWS(); await refreshStatus(); }
      } catch (_) { /* */ }
    } else {
      $("#espResult").textContent = `失敗：${JSON.stringify(msg)}`;
      toast(`ESPTOOL 失敗：${JSON.stringify(msg)}`, "err", 9000);
      setConnected(false);
    }
    $("#led").dataset.state = state.connected ? "idle" : "error";
    setTimeout(() => $("#espProgressWrap").classList.add("hidden"), 1200);
  };
  xhr.onerror = () => {
    $("#btnEspFlash").disabled = false;
    setConnected(false);
    toast("ESPTOOL 網絡錯誤", "err");
  };
  xhr.send(erase ? new Blob([]) : state.espFile);
}
