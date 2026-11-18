/* ESP WebMaster — 環境（doctor／安裝）
 *
 * 只經 REST：`GET /api/env`（報告）、`POST /api/env/install`（要 confirm）、
 * `GET /api/env/install?since=N`（逐次讀新行）。
 *
 * 為什麼唔喺前端做探測：瀏覽器睇唔到本機裝咗咩（冇檔案系統）。所有探測都在
 * server（env.py），前端只負責顯示＋觸發。
 */
"use strict";

const ENV_READY_LABEL = {
  run_server: "開 server",
  flash_nc4_ota: "NC4 OTA 燒錄",
  flash_esptool: "server 側 esptool",
  flash_browser: "頁面燒錄（瀏覽器）",
  build_firmware: "建置 firmware",
};

let envReport = null;
let envLogLines = 0;
let envPollTimer = null;

function envChip(status) {
  const map = { ok: "ok", warn: "amber", missing: "err" };
  return map[status] || "";
}

function envRenderReady(data) {
  const grid = $("#envReadyGrid");
  if (!grid) return;
  const ready = data.ready || {};
  grid.innerHTML = "";
  for (const [k, v] of Object.entries(ready)) {
    const item = document.createElement("div");
    item.className = "cfg-item";
    const kk = document.createElement("div");
    kk.className = "k";
    kk.textContent = ENV_READY_LABEL[k] || k;
    const vv = document.createElement("div");
    vv.className = "v " + (v ? "on" : "warn");
    vv.textContent = v ? "可用" : "未齊";
    item.append(kk, vv);
    grid.appendChild(item);
  }
  const miss = data.missing || [];
  const warn = data.warn || [];
  $("#envSummary").textContent = miss.length
    ? `缺 ${miss.length} 項`
    : (warn.length ? `注意 ${warn.length} 項` : "全部就緒");
  $("#envMissing").textContent = miss.length ? "缺：" + miss.join("／") : "—";
  $("#envOsLine").textContent =
    `${(data.os && data.os.platform) || ""} ｜ Python：${(data.os && data.os.pythonExe) || ""}`
    + ` ｜ IDF 預設路徑：${data.installDir || "—"}`
    + ((data.idfCandidates && data.idfCandidates.length)
        ? ` ｜ 已找到：${data.idfCandidates[0]}` : "");
  const inp = $("#envDir");
  if (inp && !inp.value && data.installDir) inp.placeholder = data.installDir;
}

function envRenderTools(data) {
  const body = $("#envToolsBody");
  if (!body) return;
  body.innerHTML = "";
  for (const t of data.tools || []) {
    const tr = document.createElement("tr");
    const td1 = document.createElement("td");
    td1.textContent = t.label || t.id;
    td1.title = t.id;
    const td2 = document.createElement("td");
    const chip = document.createElement("span");
    chip.className = "chip " + envChip(t.status);
    chip.textContent = t.status === "ok" ? "OK" : (t.status === "warn" ? "注意" : "缺");
    td2.appendChild(chip);
    const td3 = document.createElement("td");
    td3.className = "mono";
    td3.style.fontSize = "11px";
    td3.textContent = t.version || t.detail || "—";
    if (t.note) td3.title = t.note;
    const td4 = document.createElement("td");
    td4.className = "small";
    if (t.status === "ok") {
      const bits = [];
      if (t.updateStatus) bits.push(t.updateStatus);
      if (t.note) bits.push(t.note);
      td4.textContent = bits.join("　");
      const act = envRowAction(t);          // 「更新」按鈕
      if (act && (t.outdated || /可更新/.test(t.updateStatus || ""))) {
        td4.appendChild(document.createTextNode(" "));
        td4.appendChild(act);
      }
    } else {
      td4.textContent = [t.impact ? `後果：${t.impact}` : "",
                        t.fix ? `→ ${t.fix}` : ""]
                        .filter(Boolean).join("　");
      const act = envRowAction(t);          // 「安裝」按鈕
      if (act) {
        td4.appendChild(document.createTextNode(" "));
        td4.appendChild(act);
      }
    }
    tr.append(td1, td2, td3, td4);
    body.appendChild(tr);
  }
}

/**
 * 顯示「開機自動安裝」到底會做咩、上次做成點、為何冇做。
 * 為什麼要這個：用戶見到「ESP-IDF 缺」但冇被裝，會以為程式冇用——
 * 其實原因（例如所有候選位置都唔可寫）就在 auto.plan 的 hint 裡。
 */
function envRenderAuto(data) {
  const box = $("#envAutoBody");
  const chip = $("#envAutoMode");
  if (!box) return;
  const auto = data.auto || {};
  const mode = auto.mode || "?";
  const modeText = {
    off: "off（只檢查）",
    safe: "safe（自動補 deps/esptool）",
    all: "all（含 ESP-IDF）",
  }[mode] || mode;
  if (chip) chip.textContent = modeText;

  const lines = [];
  const plan = auto.plan || [];
  if (mode === "off") {
    lines.push("設定係 off：只檢查、唔會自動安裝。想開機就裝 → settings.json 的 env.auto_install 改成 \"all\"。");
  } else if (!plan.length) {
    lines.push("冇需要自動安裝的項目（現有環境已滿足，或 6 小時內試過失敗而被冷卻）。");
  } else {
    for (const it of plan) {
      if (it.blocked) {
        lines.push(`❌ 唔可以自動補 ${it.what}：${it.reason}`);
        if (it.hint) lines.push(`　→ ${it.hint}`);
      } else {
        lines.push(`⚙ 開機時會自動補：${it.what}（${it.reason}）`);
      }
    }
  }
  const last = auto.last || {};
  const keys = Object.keys(last);
  if (keys.length) {
    lines.push("上次自動安裝：");
    for (const k of keys) {
      const v = last[k] || {};
      const when = v.at ? new Date(v.at * 1000).toLocaleString() : "?";
      lines.push(`　${k}：${v.ok ? "成功" : "失敗"}（${when}）`);
    }
  }
  if (data.updates && data.updates.length) {
    const bad = data.updates.filter((u) => u.outdated);
    lines.push(bad.length
      ? "套件可更新：" + bad.map((u) => `${u.pkg} ${u.installed}→${u.latest}`).join("、")
      : "套件版本：全部已是最新");
  }
  box.innerHTML = "";
  for (const line of lines) {
    const div = document.createElement("div");
    div.textContent = line;
    div.style.marginBottom = "3px";
    box.appendChild(div);
  }
}

/** 為工具列加「修正」按鈕：直接帶到安裝卡並預選項目。 */
function envRowAction(tool) {
  const btn = document.createElement("button");
  btn.className = "btn sm";
  const map = {
    idf: "idf", idf_toolchain: "idf",
    esptool: "esptool",
    deps: "deps",
  };
  const what = map[tool.id];
  if (!what) return null;
  const outdated = tool.outdated || tool.updateStatus && /可更新/.test(tool.updateStatus);
  btn.textContent = outdated ? "更新" : "安裝";
  btn.onclick = () => {
    const sel = $("#envWhat");
    if (sel) { sel.value = what; sel.dispatchEvent(new Event("change")); }
    const card = $("#envWhat");
    if (card) card.scrollIntoView({ block: "center", behavior: "smooth" });
    toast(`已選好「${what}」，確認後按「開始安裝」`, "ok", 3000);
  };
  return btn;
}

/** 「用可寫位置」：把第一個真正可寫的候選填落輸入框（實測過才填）。 */
function envPickWritable() {
  const data = envReport || {};
  const cands = data.installDirCandidates || [];
  const ok = cands.filter((c) => c.writable);
  const hint = $("#envCfgHint");
  if (!ok.length) {
    if (hint) {
      hint.textContent = "冇一個候選可寫（權限問題）——請手動填一個你寫得入的路徑";
      hint.className = "small err";
    }
    toast("冇可寫位置：請手動指定一個資料夾（例如 D:\\esp\\esp-idf）", "warn", 8000);
    return;
  }
  $("#envDir").value = ok[0].path;
  if (hint) { hint.textContent = "已填入可寫位置：" + ok[0].path; hint.className = "small ok"; }
}

/** 儲存 env.* 到 settings.local.json（本機覆寫；唔會改團隊 settings.json）。 */
async function envSaveConfig() {
  const body = {
    idf_targets: envPickedTargets(),
  };
  const dir = $("#envDir").value.trim();
  if (dir) body.idf_dir = dir;
  const ver = $("#envVer") ? $("#envVer").value : "";
  if (ver) body.idf_version = ver;
  if (!$("#envWhat")) return;
  try {
    const j = await api("/api/env/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    toast(`已寫入 ${j.file}；重開 server 後生效（或按「重新檢查」看現況）`, "ok", 6000);
    const hint = $("#envCfgHint");
    if (hint) { hint.textContent = `已儲存到 ${j.file}`; hint.className = "small ok"; }
    envLoad();
  } catch (e) {
    toast(`儲存失敗：${e.message}`, "err", 8000);
  }
}

async function envLoad() {
  try {
    const j = await api("/api/env");
    envReport = j.data;
    envRenderReady(envReport);
    envRenderTools(envReport);
    envRenderAuto(envReport);
    if (!envTargetsRendered) { envRenderTargets(envReport); envTargetsRendered = true; }
  } catch (e) {
    $("#envSummary").textContent = "讀唔到";
    $("#envToolsBody").innerHTML =
      `<tr><td colspan="4" class="muted">環境檢查失敗：${e.message}</td></tr>`;
  }
}

function envLogAppend(lines) {
  const box = $("#envLog");
  if (!box) return;
  for (const line of lines || []) {
    const div = document.createElement("div");
    div.textContent = line;
    div.className = "line" + (/\[(錯誤|例外)\]/.test(line) ? " err" : "");
    box.appendChild(div);
  }
  while (box.childElementCount > 3000) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}

async function envPollJob() {
  try {
    const j = await api(`/api/env/install?since=${envLogLines}`);
    const d = j.data;
    envLogAppend(d.lines);
    envLogLines = d.total;
    const state = $("#envJobState");
    if (d.running) {
      state.textContent = `進行中（${d.what}）${d.elapsed != null ? " " + d.elapsed + "s" : ""}`;
      state.className = "chip amber";
      const wrap = $("#envProgressWrap");
      if (wrap) wrap.classList.remove("hidden");
      $("#btnEnvInstall").disabled = true;
    } else {
      if (true) {
        const wrap = $("#envProgressWrap");
        if (wrap) wrap.classList.add("hidden");
      }
      $("#btnEnvInstall").disabled = false;
      if (d.rc === null || d.rc === undefined) {
        state.textContent = "閒置";
        state.className = "chip";
      } else if (d.rc === 0) {
        state.textContent = "完成";
        state.className = "chip ok";
        toast("安裝完成", "ok", 4000);
        envLoad();                       // 重新檢查
      } else {
        state.textContent = `失敗（rc=${d.rc}）`;
        state.className = "chip err";
        toast(`安裝失敗（rc=${d.rc}），睇日誌`, "err", 9000);
      }
      if (envPollTimer) { clearInterval(envPollTimer); envPollTimer = null; }
    }
  } catch (e) {
    // 網絡／server 重啟：停止 poll，避免刷屏
    if (envPollTimer) { clearInterval(envPollTimer); envPollTimer = null; }
    $("#envJobState").textContent = "讀唔到狀態";
  }
}

let envVersionsLoaded = false;
let envTargetsRendered = false;

/** 由 server 取真實的 ESP-IDF tag 清單（唔好寫死：v6.1.0 根本唔存在）。 */
async function envLoadVersions(force = false) {
  const sel = $("#envVer");
  if (!sel) return;
  if (envVersionsLoaded && !force) return;
  try {
    const j = await api("/api/env/idf/versions?force=" + (force ? 1 : 0));
    const d = j.data || {};
    sel.innerHTML = "";
    const latest = document.createElement("option");
    latest.value = "";
    latest.textContent = "最新正式版" + (d.latest ? `（${d.latest}）` : "");
    sel.appendChild(latest);
    for (const v of (d.versions || [])) {
      const o = document.createElement("option");
      o.value = v;
      o.textContent = v;
      sel.appendChild(o);
    }
    envVersionsLoaded = true;
    if (d.error) {
      sel.title = "取版本清單失敗：" + d.error;
      toast("取 IDF 版本清單失敗（可能離線），仍可用「最新正式版」", "warn", 6000);
    }
    // 記住上次選擇
    try {
      const saved = localStorage.getItem("espwm.idfVer");
      if (saved && [].some.call(sel.options, (o) => o.value === saved)) sel.value = saved;
    } catch (_) { /* */ }
  } catch (e) {
    sel.title = "取版本清單失敗：" + e.message;
  }
}

/**
 * 依 /api/env 的「權威清單」畫 chip（esp32s3／esp32s31／esp32c5… 自動出現）。
 * 為什麼要動態：晶片會一直出新（S31、C5、C61、H4、H21 都是後來加的），
 * 前端寫死一定會過時——實測連我自己的硬編碼清單都漏了 esp32s31。
 */
function envRenderTargets(data) {
  const box = $("#envTargets");
  if (!box) return;
  const t = data.targets || {};
  const live = t.live || Object.keys(t.known || {});
  const known = t.known || {};
  // 優先：用戶之前在 UI 揀的 → 設定檔的 env.idf_targets → 伺服器預設
  const picked = envPickedTargets();
  const wanted = new Set(picked.length ? picked
                        : ((t.configured && t.configured.length)
                            ? t.configured : (t.default || [])));
  box.innerHTML = "";
  for (const name of live) {
    const label = document.createElement("label");
    label.className = "chip";
    const cb = document.createElement("input");
    cb.type = "checkbox";
    cb.value = name;
    cb.checked = wanted.has(name);
    const span = document.createElement("span");
    // 有中文名就用（例如 ESP32-S31），否則用原始 target 名
    span.textContent = (known[name] || name).replace(/^ESP32-?/, "");
    label.title = name;
    label.append(cb, span);
    box.appendChild(label);
  }
  const src = $("#envTgtSource");
  if (src) {
    const mode = (data.auto && data.auto.mode) || "?";
    const modeText = { off: "只檢查", safe: "自動補 deps/esptool", all: "開機自動裝（含 IDF）" }[mode] || mode;
    src.textContent = `清單來源：${t.source || "?"}（共 ${live.length} 款）`
      + `　｜　開機自動安裝：${modeText}`;
  }
  envTargetsHint();
}

/** 收集 UI 上勾選的晶片 → ["esp32s3", …]（空＝用伺服器預設）。 */
function envPickedTargets() {
  const box = $("#envTargets");
  if (!box) return [];
  return [].map.call(box.querySelectorAll('input[type="checkbox"]:checked'),
                     (i) => i.value);
}

function envSetTargets(list) {
  const box = $("#envTargets");
  if (!box) return;
  const want = new Set(list || []);
  [].forEach.call(box.querySelectorAll('input[type="checkbox"]'), (i) => {
    i.checked = want.has(i.value);
  });
  envTargetsHint();
}

function envTargetsHint() {
  const hint = $("#envTgtHint");
  if (!hint) return;
  const picked = envPickedTargets();
  hint.textContent = picked.length
    ? `${picked.length} 個：${picked.join("／")}`
    : "冇揀 → 用預設（S3／C3／C6／P4）";
}

async function envInstall() {
  const what = $("#envWhat").value;
  const picked = envPickedTargets();
  const target = picked.join(",");        // 空字串＝server 用預設（DEFAULT_TARGETS）
  const dir = $("#envDir").value.trim();
  const labels = { idf: "ESP-IDF（clone＋install）", deps: "Web deps", esptool: "esptool" };
  const verSel = $("#envVer");
  const ver = verSel ? verSel.value : "";
  const tgtText = picked.length ? picked.join("／") : "預設（S3／C3／C6／P4）";
  const verText = what === "idf"
    ? `IDF 版本：${ver || "最新正式版"}\n目標晶片：${tgtText}\n` : "";
  const cost = verText + (what === "idf"
    ? "會 clone 官方 repo（約 1–2 GB）並裝工具鏈，**10–30 分鐘**，期間佔用頻寬。"
    : "會跑 pip install（幾十秒）。");
  if (!confirm(`開始安裝：${labels[what] || what}？\n\n${cost}\n\n輸出會顯示喺下面日誌。`)) {
    return;
  }
  $("#envLog").textContent = "";
  envLogLines = 0;
  $("#btnEnvInstall").disabled = true;
  try {
    const body = { what, target, confirm: true };
    if (dir) body.dir = dir;
    if (ver) body.version = ver;
    try { localStorage.setItem("espwm.idfVer", ver); } catch (_) { /* */ }
    await api("/api/env/install", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    envLogAppend([`已開始安裝：${labels[what] || what}`]);
    if (envPollTimer) clearInterval(envPollTimer);
    envPollTimer = setInterval(envPollJob, 1200);
    envPollJob();
  } catch (e) {
    $("#btnEnvInstall").disabled = false;
    toast(`開始失敗：${e.message}`, "err", 8000);
    envLogAppend([`開始失敗：${e.message}`]);
  }
}

function bindEnv() {
  if (!$("#btnEnvRefresh")) return;
  $("#btnEnvRefresh").onclick = () => { toast("重新檢查…", "ok", 1200); envLoad(); };
  $("#btnEnvInstall").onclick = envInstall;
  const pick = $("#btnEnvPickWritable");
  if (pick) pick.onclick = envPickWritable;
  const save = $("#btnEnvSaveCfg");
  if (save) save.onclick = envSaveConfig;
  $("#btnEnvClearLog").onclick = () => {
    $("#envLog").textContent = "（已清）";
    envLogLines = 0;
  };
  // 版本清單：由 server 拉真實 tag（v6.1.0 唔存在，所以唔可以寫死）
  const verReload = $("#btnEnvVerReload");
  if (verReload) {
    verReload.onclick = () => { envVersionsLoaded = false; envLoadVersions(true); };
  }
  // 晶片多選
  const tgtBox = $("#envTargets");
  if (tgtBox) {
    tgtBox.addEventListener("change", envTargetsHint);
    envTargetsHint();
  }
  const scp = $("#btnEnvTgtScp");
  if (scp) scp.onclick = () => envSetTargets(["esp32s3", "esp32c3", "esp32c6", "esp32p4"]);
  const all = $("#btnEnvTgtAll");
  if (all) all.onclick = () => {
    const box2 = $("#envTargets");
    envSetTargets(box2
      ? [].map.call(box2.querySelectorAll('input[type="checkbox"]'), (i) => i.value)
      : []);
  };
  const none = $("#btnEnvTgtNone");
  if (none) none.onclick = () => envSetTargets([]);

  // 進頁時檢查一次（唔用 poll；只有安裝中才 poll）
  $("#envWhat").onchange = () => {
    const idf = $("#envWhat").value === "idf";
    if (tgtBox) tgtBox.style.opacity = idf ? "1" : ".45";
    $("#envDir").disabled = !idf;
    const ver = $("#envVer");
    if (ver) ver.disabled = !idf;
    if (idf) envLoadVersions();
  };
  $("#envWhat").onchange();
}
