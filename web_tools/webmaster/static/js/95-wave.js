/* ESP WebMaster — 波形模擬器（六工具合成）
 *
 * 讀 effects.json（或單一效果／純 program 陣列），用同 firmware 一樣嘅公式
 * （nc_px_engine_program ＝ MicroPython PixelMathMethod）算波表，前端畫：
 *   上：整條燈鏈（pixel 亮度 = 曲線值 + spacing 相位）
 *   下：時間曲線（段邊界虛線、目前 step 藍線）
 * 入口：
 *   1. 左邊「波形」頁：貼 JSON／讀裝置檔案／開本機檔案 → 解析
 *   2. IDE：開檔之後按工具列「🌊 波形」直接跳過來
 */
"use strict";

const WaveSim = (function () {
  const KIND = { keep: 0, math_now: 1, square_wave_now: 2, pulse_wave: 3, pulse: 4, starter: 5 };
  const TOOLS = Object.keys(KIND);

  function clamp12(v) {
    v = Math.trunc(Number(v) || 0);
    return v < 0 ? 0 : (v > 4095 ? 4095 : v);
  }

  // 0..65535 相位 → 0..4095（拋物線＋二次修正，同 C nc_px_engine_sin_q12 一致）
  function wave01(phase) {
    phase = phase & 65535;
    let x, sgn;
    if (phase < 32768) { x = phase; sgn = 1; } else { x = phase - 32768; sgn = -1; }
    let s = (x * (32768 - x)) >> 16;
    const s2 = (s * s) >> 12;
    s = s + ((922 * (s2 - s)) >> 12);
    if (sgn < 0) { s = -s; }
    const w = (s + 4096) >> 1;
    return w > 4095 ? 4095 : w;
  }

  function compile(program) {
    const comp = [];
    let prev = 0;
    program.forEach((sg) => {
      let end = Math.trunc(Number(sg.end_Time) || 0);
      let fs = end - prev;
      if (fs < 1) { fs = 1; }
      const lMax = clamp12(sg.l_max == null ? 4095 : sg.l_max);
      const lLim = clamp12(sg.l_lim == null ? 0 : sg.l_lim);
      let lRange = lMax - lLim;
      if (lRange < 0) { lRange = 0; }
      const F = Math.trunc(Number(sg.F) || 1);
      const stepPhase = Math.floor((65536 * F) / 10 / fs);
      const phi4 = clamp12(sg.phi == null ? 0 : sg.phi) << 4;
      const kind = KIND[sg.type] != null ? KIND[sg.type] : 0;
      const pulse = Math.trunc(Number(sg.pulse == null ? 2047 : sg.pulse));
      let gap = F > 0 ? Math.floor(fs / F) : 1;
      if (gap < 1) { gap = 1; }
      const width = pulse % gap;
      comp.push({ start: prev, end: end, kind: kind, l_range: lRange, l_lim: lLim,
                  phi4: phi4, step_phase: stepPhase, pulse: pulse, gap: gap, width: width });
      prev = end;
    });
    return { segs: comp, total: prev };
  }

  function valueAt(c, g) {
    const segs = c.segs, total = c.total;
    if (!segs.length || total <= 0) { return 0; }
    g = Math.trunc(g);
    if (g < 0 || g >= total) { g %= total; if (g < 0) { g += total; } }
    for (let i = 0; i < segs.length; i++) {
      const s = segs[i];
      if (g >= s.end) { continue; }
      if (s.kind === 0) { return s.l_range + s.l_lim; }
      if (s.kind === 5) { return 0; }
      const rel = g - s.start;
      if (s.kind === 4) {
        return s.l_lim + (((rel + s.phi4) % s.gap) <= s.width ? s.l_range : 0);
      }
      const ph = (s.phi4 + s.step_phase * rel) % 65536;
      const w = wave01(ph);
      let v;
      if (s.kind === 1) { v = (w * s.l_range) >> 12; }
      else if (s.kind === 2) { v = w >= 2048 ? s.l_range : 0; }
      else { v = w >= s.pulse ? s.l_range : 0; }
      return v + s.l_lim;
    }
    return 0;
  }

  // ── 狀態 ────────────────────────────────────────────
  const S = { effects: [], cur: null, comp: null, wave: [], step: 0, timer: null };

  function normalize(e, fallbackName) {
    const prog = Array.isArray(e.program) ? e.program
      : (Array.isArray(e) ? e : null);
    if (!prog) { return null; }
    return {
      id: e.id == null ? "" : e.id,
      name: e.name || fallbackName || "effect",
      pixel_n: Math.max(1, Math.trunc(Number(e.pixel_n) || 64)),
      step: Math.trunc(Number(e.step == null ? 1 : e.step)),
      spacing: Math.trunc(Number(e.spacing) || 0),
      offset: Math.trunc(Number(e.offset) || 0),
      speed: Math.max(1, Math.trunc(Number(e.speed) || 1)),
      reverse: !!e.reverse,
      program: prog,
    };
  }

  function parseText(text) {
    const t = String(text || "").trim();
    if (!t) { throw new Error("冇內容"); }
    let data;
    try { data = JSON.parse(t); } catch (e) { throw new Error("JSON 解析失敗：" + e.message); }
    const list = [];
    if (Array.isArray(data)) {
      const e = normalize({ program: data }, "program");
      if (e) { list.push(e); }
    } else if (data && Array.isArray(data.effects)) {
      data.effects.forEach((x) => { const e = normalize(x); if (e) { list.push(e); } });
    } else if (data && Array.isArray(data.program)) {
      const e = normalize(data);
      if (e) { list.push(e); }
    } else if (data && typeof data === "object") {
      const e = normalize(data, "effect");
      if (e) { list.push(e); }
    }
    if (!list.length) { throw new Error("搵唔到 program（要 effects[]、單一效果、或段陣列）"); }
    return list;
  }

  function compute(e) {
    const c = compile(e.program);
    const wave = [];
    for (let g = 0; g < c.total; g++) { wave.push(valueAt(c, g)); }
    return { comp: c, wave: wave };
  }

  // ── 畫 ──────────────────────────────────────────────
  function drawStrip() {
    const cv = document.querySelector("#waveStrip");
    if (!cv || !S.cur) { return; }
    const ctx = cv.getContext("2d");
    const W = cv.width, H = cv.height;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = "#0b0d10"; ctx.fillRect(0, 0, W, H);
    if (!S.wave.length) { return; }
    const e = S.cur, n = e.pixel_n, cw = W / n;
    const base = ((Math.floor(S.step / e.speed) * e.step + e.offset) % S.comp.total
      + S.comp.total) % S.comp.total;
    for (let p = 0; p < n; p++) {
      const ph = ((base + p * e.spacing) % S.comp.total + S.comp.total) % S.comp.total;
      const t = (S.wave[ph] || 0) / 4095;
      ctx.fillStyle = "rgb(" + Math.round(255 * t) + "," + Math.round(190 * t) + "," +
        Math.round(120 * t) + ")";
      const x = e.reverse ? (n - 1 - p) : p;
      ctx.fillRect(Math.floor(x * cw) + 1, 10, Math.max(1, Math.floor(cw) - 2), H - 20);
    }
    ctx.strokeStyle = "rgba(255,255,255,.08)";
    for (let p = 8; p < n; p += 8) {
      const x = Math.floor((e.reverse ? (n - p) : p) * cw);
      ctx.beginPath(); ctx.moveTo(x, 6); ctx.lineTo(x, H - 6); ctx.stroke();
    }
  }

  function drawCurve() {
    const cv = document.querySelector("#waveCurve");
    if (!cv || !S.cur) { return; }
    const ctx = cv.getContext("2d");
    const W = cv.width, H = cv.height, pad = 30;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = "#0b0d10"; ctx.fillRect(0, 0, W, H);
    if (!S.wave.length) {
      ctx.fillStyle = "#8b96a5"; ctx.font = "14px ui-monospace";
      ctx.fillText("（冇六工具 program）", pad, H / 2);
      return;
    }
    const w = S.wave, total = S.comp.total || 1;
    let vmax = 1;
    for (let i = 0; i < w.length; i++) { if (w[i] > vmax) { vmax = w[i]; } }
    ctx.font = "11px ui-monospace";
    for (let i = 0; i <= 4; i++) {
      const y = pad + (H - 2 * pad) * i / 4;
      ctx.strokeStyle = "rgba(255,255,255,.12)";
      ctx.beginPath(); ctx.moveTo(pad, y); ctx.lineTo(W - 8, y); ctx.stroke();
      ctx.fillStyle = "#8b96a5";
      ctx.fillText(String(Math.round(vmax * (4 - i) / 4)), 4, y + 4);
    }
    const xw = W - 8 - pad;
    ctx.strokeStyle = "#ffb35c"; ctx.lineWidth = 2; ctx.beginPath();
    for (let x = 0; x <= xw; x++) {
      const g = Math.min(total - 1, Math.floor(x * total / Math.max(1, xw)));
      const y = pad + (H - 2 * pad) * (1 - (w[g] || 0) / vmax);
      if (x === 0) { ctx.moveTo(pad, y); } else { ctx.lineTo(pad + x, y); }
    }
    ctx.stroke();
    ctx.setLineDash([4, 4]); ctx.strokeStyle = "rgba(255,255,255,.35)";
    S.comp.segs.forEach((sg) => {
      if (sg.end > 0 && sg.end < total) {
        const x = pad + xw * sg.end / total;
        ctx.beginPath(); ctx.moveTo(x, pad); ctx.lineTo(x, H - pad); ctx.stroke();
      }
    });
    ctx.setLineDash([]);
    const x = pad + xw * Math.min(total, S.step) / total;
    ctx.strokeStyle = "#5cc8ff"; ctx.beginPath();
    ctx.moveTo(x, pad); ctx.lineTo(x, H - pad); ctx.stroke();
  }

  function renderSegs() {
    const el = document.querySelector("#waveSegs");
    if (!el || !S.cur) { return; }
    const lines = [];
    let prev = 0;
    S.comp.segs.forEach((sg, i) => {
      lines.push((i + 1) + ". " + (S.cur.program[i].type || "keep") + "　" +
        (sg.end - prev) + " 步　" + sg.l_lim + ".." + (sg.l_range + sg.l_lim) +
        "　F=" + (S.cur.program[i].F == null ? "-" : S.cur.program[i].F));
      prev = sg.end;
    });
    el.textContent = lines.join("\n") || "（冇段）";
  }

  function renderAll() {
    const info = document.querySelector("#waveInfo");
    if (info && S.cur) {
      info.textContent = S.cur.name + " · " + S.cur.pixel_n + " px · total " +
        S.comp.total + " · spacing=" + S.cur.spacing + " speed=" + S.cur.speed;
    }
    drawStrip(); drawCurve(); renderSegs();
  }

  function selectEffect(i) {
    if (!S.effects[i]) { return; }
    S.cur = S.effects[i];
    S.step = 0;
    const c = compute(S.cur);
    S.comp = c.comp; S.wave = c.wave;
    renderAll();
  }

  function loadText(text, hintName) {
    const list = parseText(text);
    if (hintName && list.length === 1) { list[0].name = hintName; }
    S.effects = list;
    const sel = document.querySelector("#waveEffect");
    if (sel) {
      sel.innerHTML = "";
      list.forEach((e, i) => {
        const o = document.createElement("option");
        o.value = String(i);
        o.textContent = (e.id ? e.id + ". " : "") + e.name;
        sel.appendChild(o);
      });
      sel.value = "0";
      sel.onchange = () => selectEffect(parseInt(sel.value, 10));
    }
    selectEffect(0);
    return list.length;
  }

  function tick() {
    if (!S.comp || !S.comp.total) { return; }
    S.step = (S.step + 1) % S.comp.total;
    drawStrip(); drawCurve();
  }

  function play(on) {
    if (S.timer) { clearInterval(S.timer); S.timer = null; }
    if (on) { S.timer = setInterval(tick, 20); }
    const b = document.querySelector("#btnWavePlay");
    if (b) { b.textContent = on ? "⏸ 暫停" : "▶ 播放"; }
  }

  function bind() {
    const parseBtn = document.querySelector("#btnWaveParse");
    if (parseBtn) {
      parseBtn.onclick = () => {
        try {
          const n = loadText(document.querySelector("#waveJson").value);
          toast("已載入 " + n + " 條效果", "ok");
          switchView("wave");
        } catch (e) { toast("解析失敗：" + e.message, "err"); }
      };
    }
    const playBtn = document.querySelector("#btnWavePlay");
    if (playBtn) { playBtn.onclick = () => play(!S.timer); }
    const zero = document.querySelector("#btnWaveZero");
    if (zero) { zero.onclick = () => { S.step = 0; drawStrip(); drawCurve(); }; }
    const openFile = document.querySelector("#btnWaveOpenFile");
    const fileInput = document.querySelector("#waveFile");
    if (openFile && fileInput) {
      openFile.onclick = () => fileInput.click();
      fileInput.onchange = () => {
        const f = fileInput.files && fileInput.files[0];
        if (!f) { return; }
        const rd = new FileReader();
        rd.onload = () => {
          document.querySelector("#waveJson").value = rd.result;
          try { loadText(rd.result, f.name); toast("已載入 " + f.name, "ok"); }
          catch (e) { toast("解析失敗：" + e.message, "err"); }
        };
        rd.readAsText(f);
      };
    }
    const readDev = document.querySelector("#btnWaveRead");
    if (readDev) {
      readDev.onclick = async () => {
        const path = (document.querySelector("#wavePath").value || "").trim();
        if (!path) { toast("請填裝置檔案路徑（例如 /pixel/effects/effects.json）", "warn"); return; }
        try {
          const r = await fetch("/api/file?path=" + encodeURIComponent(path));
          if (!r.ok) { throw new Error("HTTP " + r.status); }
          const text = await r.text();
          document.querySelector("#waveJson").value = text;
          loadText(text, path.split("/").pop());
          toast("已讀取 " + path, "ok");
        } catch (e) { toast("讀取失敗：" + e.message, "err"); }
      };
    }
    // IDE 工具列：把目前編輯器內容送入波形模擬器
    const ideBtn = document.querySelector("#btnIdeWave");
    if (ideBtn) {
      ideBtn.onclick = () => {
        const cm = (typeof ideEnsureCM === "function") ? ideEnsureCM() : state.ideCM;
        const tab = state.ideTabs && state.ideTabs.find((t) => t.path === state.ideActive);
        const text = cm ? cm.getValue() : (tab ? tab.text : "");
        if (!text) { toast("IDE 未開啟檔案", "warn"); return; }
        if (tab && tab.path && tab.path.endsWith(".bin")) {
          toast("二進位檔唔可以模擬", "warn"); return;
        }
        document.querySelector("#waveJson").value = text;
        try { loadText(text, tab ? tab.path.split("/").pop() : "ide"); } catch (e) {
          toast("解析失敗：" + e.message, "err"); return;
        }
        switchView("wave");
      };
    }
  }

  document.addEventListener("DOMContentLoaded", () => setTimeout(bind, 0));

  return { loadText: loadText, play: play, tick: tick };
})();
