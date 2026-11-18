/* ESP WebMaster — 指令頁：NC4 console 與快速指令
 * 任意 cmd + hex payload + expect；parseStatusAck／statusSet。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

// ── Console / quick command ───────────────────────────
async function loadCommands() {
  try {
    const j = await api("/api/commands");
    state.commands = j.data || [];
    const sel = $("#cmdSelect");
    sel.innerHTML = "";
    for (const c of state.commands) {
      const o = document.createElement("option");
      o.value = c.cmd;
      o.textContent = `${c.cmd}  ${c.name}`;
      sel.appendChild(o);
    }
    sel.value = "0x1101";
  } catch (e) {
    console.warn("commands:", e);
  }
}

async function sendCmd(cmd, payloadHex, expect, timeout = 5) {
  const body = {
    cmd,
    payload_hex: payloadHex || "",
    expect: expect || null,
    timeout: Number(timeout) || 5,
  };
  return api("/api/cmd", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
}

async function onSendCmd() {
  const cmd = $("#cmdSelect").value || "0x1101";
  let payload;
  try { payload = hexToBytes($("#cmdPayload").value); }
  catch (e) { toast(e.message, "err"); return; }
  const expText = $("#cmdExpect").value.trim();
  const expect = expText ? expText.split(",").map((s) => s.trim()).filter(Boolean) : null;
  const hex = Array.from(payload).map((b) => b.toString(16).padStart(2, "0")).join("");
  $("#cmdResult").textContent = "等待回覆…";
  try {
    const j = await sendCmd(cmd, hex, expect, $("#cmdTimeout").value);
    const jsonText = j.json ? JSON.stringify(j.json, null, 2) : "（唔係 JSON）";
    $("#cmdResult").textContent = `cmd ${j.cmd}  len ${j.len}\n${jsonText}\n\nhex:\n${j.hex}`;
    logLine(`cmd ${cmd} → ${j.cmd}（${j.len} B）`, "ok");
  } catch (e) {
    $("#cmdResult").textContent = `失敗：${e.message}`;
    logLine(`cmd ${cmd} 失敗：${e.message}`, "err");
  }
}

async function onQuickSend() {
  const raw = $("#quickCmd").value.trim();
  let cmd = "0x1101";
  let payload = "";
  if (raw) {
    const parts = raw.split(/\s+/);
    if (!/^0x[0-9a-fA-F]+$/.test(parts[0])) {
      toast("格式：0xCMD [hex payload]", "warn");
      return;
    }
    cmd = parts[0];
    payload = (parts[1] || "").replace(/[^0-9a-fA-F]/g, "");
  }
  try {
    const j = await sendCmd(cmd, payload, null, 6);
    logLine(`>>> ${cmd} ${payload}`.trim(), "dim");
    if (j.json) logLine(JSON.stringify(j.json, null, 2), "ok");
    else logLine(`${j.cmd}  ${j.len} B  ${j.hex}`, "ok");
    $("#quickCmd").value = "";
  } catch (e) {
    logLine(`>>> ${cmd} 失敗：${e.message}`, "err");
  }
}

function parseStatusAck(hex) {
  const clean = (hex || "").replace(/[^0-9a-fA-F]/g, "");
  const b = new Uint8Array(clean.length / 2);
  for (let i = 0; i < b.length; i++) b[i] = parseInt(clean.slice(i * 2, i * 2 + 2), 16);
  if (b.length < 3) return { ok: false, message: "" };
  const n = b[1] | (b[2] << 8);
  const msg = new TextDecoder("utf-8").decode(b.slice(3, 3 + Math.min(n, b.length - 3)));
  return { ok: b[0] === 1, message: msg };
}

function statusSet(obj, label, persist = 0) {
  const json = new TextEncoder().encode(JSON.stringify(obj));
  const payload = new Uint8Array(2 + json.length + 1);
  payload[0] = json.length & 0xFF;
  payload[1] = (json.length >> 8) & 0xFF;
  payload.set(json, 2);
  payload[2 + json.length] = persist ? 1 : 0;
  const hex = Array.from(payload).map((b) => b.toString(16).padStart(2, "0")).join("");
  return sendCmd("0x1103", hex, ["0x1104"], 5)
    .then((j) => {
      const ack = parseStatusAck(j.hex);
      const msg = ack.message || JSON.stringify(j.json ?? {});
      toast(`${label}：${msg}`, ack.ok ? "ok" : "err", 5000);
      logLine(`${label} → ${msg}`, ack.ok ? "ok" : "err");
      return refreshStatus();
    })
    .catch((e) => {
      toast(`${label}失敗：${e.message}`, "err", 7000);
    });
}
