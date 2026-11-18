/* ESP WebMaster — 終端與活動面板
 * logLine（終端輸出）、activityLine（AI 操作紀錄）、showPanelTab。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

// ── 終端 ───────────────────────────────────────────────
function logLine(text, cls = "") {
  const line = String(text ?? "");
  state.logs.push(line);
  if (state.logs.length > 4000) state.logs.splice(0, state.logs.length - 4000);
  if (state.logPaused) return;
  const box = $("#terminal");
  const div = document.createElement("div");
  let k = cls;
  if (!k) {
    if (/^E\s/.test(line)) k = "err";
    else if (/^W\s/.test(line)) k = "warn";
    else if (/^I\s/.test(line)) k = "info";
    else if (/^D\s/.test(line)) k = "dim";
  }
  div.className = `line ${k}`;
  div.textContent = line;
  box.appendChild(div);
  while (box.childElementCount > 2500) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}
// ── 活動（AI 操作）────────────────────────────────────
function activityLine(ev) {
  const box = $("#activity");
  if (!box) return;
  const line = document.createElement("div");
  const actor = ev.actor || "?";
  const cls = actor === "ai" ? "act-ai" : (ev.action === "note" ? "act-note" : "act-ui");
  line.className = `line ${cls}`;
  const ts = new Date((ev.ts || Date.now() / 1000) * 1000).toLocaleTimeString();
  if (ev.action === "note") {
    line.textContent = `${ts}  [${actor}] ✎ ${ev.text || ""}`;
  } else {
    const status = ev.status != null ? ` → ${ev.status}` : "";
    const ms = ev.ms != null ? ` ${ev.ms}ms` : "";
    line.textContent = `${ts}  [${actor}] ${ev.action || ""}${status}${ms}`;
  }
  box.appendChild(line);
  while (box.childElementCount > 800) box.removeChild(box.firstChild);
  box.scrollTop = box.scrollHeight;
}

function showPanelTab(which) {
  const term = which === "term";
  $("#tabTerm").setAttribute("aria-selected", term ? "true" : "false");
  $("#tabActivity").setAttribute("aria-selected", term ? "false" : "true");
  $("#terminal").classList.toggle("hidden", !term);
  $("#activity").classList.toggle("hidden", term);
}
