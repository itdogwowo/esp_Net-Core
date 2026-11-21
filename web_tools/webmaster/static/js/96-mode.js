/* ESP WebMaster — 模式預覽：registry → mode → map → effect 四層合成
 *
 * 為什麼要另一頁：`effects.json` 只係第一層（怎麼動）。真正上燈睇到嘅係
 *   registry.list（播邊個 mode）→ mode.map[]（group × effect × write × range）
 *   → map/*.json 的 groups[].sel（邊啲燈）→ effects.json 的 program（波形）
 * 呢頁照 pixel_task 的 scatter 語義合成：單通道寫 r/g/b/w 讀 vals[k]，
 * rgb 讀 vals[3k..3k+2]，rgbw 讀 vals[4k..]（同 pixel_layout._scatter_* 一致）。
 *
 * 8×8 燈泡模擬器：用 composeFrame() 的 64 px 畫 canvas 圓形燈泡；
 * `modeWhite` 做 white mix（同 firmware 一樣：out = out*(100-w)/100 + 255*w/100，
 * R/G/B/W 四通道都做）。「讀取裝置」用 0xF06B SHOWFRAME（回 0xF06C）拿
 * firmware 最近一幀 64 px × 4 B 的實際 frame，parse JSON hex → 畫返出嚟。
 */
"use strict";

const ModeSim = (function () {
  const S = {
    effects: {},      // name → effect（含 program 編譯後的 comp）
    maps: {},         // name/id → map
    modes: [],        // 全部 mode
    registry: null,
    cur: null,        // 目前 mode
    entries: [],      // 解析後的組合
    step: 0,
    timer: null,
    leds: 64,
    totalF: 1,
    white: 80,        // white mix 0..100（預設 80 = 80% 白＋20% 效果）
    override: null,   // 由裝置讀回來的 frame：{frame:[64][4], seq, at}
  };

  function apiFile(path) {
    return fetch("/api/file?path=" + encodeURIComponent(path)).then((r) => {
      if (!r.ok) { throw new Error(path + " → HTTP " + r.status); }
      return r.text();
    }).then((t) => JSON.parse(t));
  }

  // Python slice 語義（夠用版：支援 a:b、a:、:b、a:b:c、單值）
  function sliceIndices(spec, count) {
    const s = String(spec == null ? ":" : spec);
    const p = s.split(":");
    let start, stop, step;
    if (p.length === 1) { start = parseInt(p[0], 10); stop = start + 1; step = 1; }
    else {
      start = p[0] === "" ? 0 : parseInt(p[0], 10);
      stop = p[1] === "" ? count : parseInt(p[1], 10);
      step = (p.length > 2 && p[2] !== "") ? parseInt(p[2], 10) : 1;
    }
    if (start < 0) { start += count; }
    if (stop < 0) { stop += count; }
    if (start < 0) { start = 0; }
    if (stop > count) { stop = count; }
    const out = [];
    if (step > 0) { for (let i = start; i < stop; i += step) { out.push(i); } }
    return out;
  }

  function resolveGroupRef(ref, mappingName) {
    // "matrix.full" / "3.full" / "full"（用 mode.mapping 做預設）
    const s = String(ref || "");
    const dot = s.indexOf(".");
    if (dot < 0) { return { mapping: mappingName, group: s }; }
    return { mapping: s.slice(0, dot), group: s.slice(dot + 1) };
  }

  function findMapping(ref) {
    const s = String(ref || "");
    for (const k of Object.keys(S.maps)) {
      const m = S.maps[k];
      if (String(m.id) === s || m.name === s) { return m; }
    }
    return null;
  }

  function findGroup(mapping, ref) {
    if (!mapping) { return null; }
    for (const g of (mapping.groups || [])) {
      if (String(g.id) === String(ref) || g.name === ref) { return g; }
    }
    return null;
  }

  function compileEffect(e) {
    if (!e || !Array.isArray(e.program)) { return null; }
    const c = WaveSim.rawCompile(e.program);   // 用 95-wave.js 同一份公式
    return { def: e, prog: c, total: c.total };
  }

  function buildEntries(mode) {
    const out = [];
    (mode.map || []).forEach((it) => {
      const ref = resolveGroupRef(it.group, mode.mapping);
      const mapping = findMapping(ref.mapping);
      const group = findGroup(mapping, ref.group);
      const eff = S.effects[it.effect] || null;
      out.push({
        raw: it, mapping: ref.mapping, group: ref.group,
        groupObj: group, effectName: it.effect, effect: eff,
        write: it.write || "w", range: it.range || ":",
        indices: sliceIndices(it.range, S.leds).map((i) => i),
      });
    });
    return out;
  }

  function totalFrames(mode) {
    let mx = 1;
    (S.entries || []).forEach((en) => {
      if (en.effect) { mx = Math.max(mx, en.effect.total); }
    });
    if (mode && mode.maxF > 0) { return mode.maxF; }
    return mx;
  }

  function effectValues(en, t) {
    const e = en.effect;
    if (!e || !e.prog || !e.total) { return null; }
    const d = e.def;
    const speed = Math.max(1, d.speed || 1);
    const base = ((Math.floor(t / speed) * (d.step == null ? 1 : d.step) + (d.offset || 0))
      % e.total + e.total) % e.total;
    const vals = [];
    const n = Math.max(1, d.pixel_n || 1);
    for (let i = 0; i < n; i++) {
      const ph = ((base + i * (d.spacing || 0)) % e.total + e.total) % e.total;
      vals.push(WaveSim.rawValue(e.prog, ph));
    }
    return vals;
  }

  // 照 pixel_layout._scatter_*：w/r/g/b 讀 vals[k]，rgb 讀 vals[3k..]，rgbw 讀 vals[4k..]
  function writeAt(out, ledIdx, vals, k, write) {
    if (!vals || !vals.length) { return; }
    const nv = vals.length;
    const px = out[ledIdx] || (out[ledIdx] = [0, 0, 0, 0]);
    const g = (i) => vals[((i % nv) + nv) % nv] >> 4;
    switch (write) {
      case "rgb":
        px[0] = g(3 * k); px[1] = g(3 * k + 1); px[2] = g(3 * k + 2); break;
      case "rgbw":
        px[0] = g(4 * k); px[1] = g(4 * k + 1); px[2] = g(4 * k + 2); px[3] = g(4 * k + 3); break;
      case "r": px[0] = g(k); break;
      case "g": px[1] = g(k); break;
      case "b": px[2] = g(k); break;
      case "w": px[3] = g(k); break;
      case "ww": px[3] = g(2 * k); break;
      case "wwww": px[3] = g(4 * k); break;
      default: px[0] = g(k); break;
    }
  }

  function composeFrame(t) {
    const out = [];
    for (let i = 0; i < S.leds; i++) { out.push([0, 0, 0, 0]); }
    S.entries.forEach((en) => {
      const vals = effectValues(en, t);
      if (!vals) { return; }
      en.indices.forEach((led, k) => {
        if (led >= 0 && led < S.leds) { writeAt(out, led, vals, k, en.write); }
      });
    });
    return out;
  }

  // ── white mix ─────────────────────────────────────────
  // 同 firmware 一樣：out = out*(100-w)/100 + 255*w/100，四通道都做。
  function mixChannel(c, w) {
    return Math.round((Number(c) || 0) * (100 - w) / 100 + 255 * w / 100);
  }

  function applyWhite(frame, w) {
    return (frame || []).map((px) => [
      mixChannel(px[0], w), mixChannel(px[1], w),
      mixChannel(px[2], w), mixChannel(px[3], w),
    ]);
  }

  // 顯示用：RGBW → RGB（W 併入三色，同 1D strip 一致；實機係 RGBW LED）。
  // 低亮度 breathe 只有 0–23/255，預覽可開 ×8 顯示增益；chip 會標明，不改變 firmware。
  function displayRGB(px) {
    const p = px || [0, 0, 0, 0];
    const gain = ($("#modeGain") && $("#modeGain").checked) ? 8 : 1;
    return [
      Math.min(255, (p[0] + p[3]) * gain),
      Math.min(255, (p[1] + p[3]) * gain),
      Math.min(255, (p[2] + p[3]) * gain),
    ];
  }

  // 目前要畫的 frame：有裝置實際 frame 就畫佢（firmware 已混白，唔再混一次），
  // 否則畫模擬合成＋white mix。
  function blankFrame() {
    const out = [];
    for (let i = 0; i < S.leds; i++) { out.push([0, 0, 0, 0]); }
    return out;
  }

  function displayFrame() {
    if (S.override) { return S.override.frame; }
    // 未載入 mode 時畫黑，不要用 white mix 把黑畫成白（會誤導成「已出 80% 白」）。
    if (!S.cur) { return blankFrame(); }
    return applyWhite(composeFrame(S.step), S.white);
  }

  function clearOverride() {
    if (!S.override) { return; }
    S.override = null;
    const chip = $("#modeFrameInfo");
    if (chip) { chip.textContent = "模擬合成"; }
  }

  // ── 畫布 ──────────────────────────────────────────────
  function drawStrip(frame) {
    const cv = $("#modeStrip");
    if (!cv) { return; }
    const ctx = cv.getContext("2d");
    const W = cv.width, H = cv.height;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = "#0b0d10"; ctx.fillRect(0, 0, W, H);
    const cw = W / S.leds;
    for (let i = 0; i < S.leds; i++) {
      const [r, g, b] = displayRGB(frame[i]);
      ctx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
      ctx.fillRect(Math.floor(i * cw) + 1, 10, Math.max(1, Math.floor(cw) - 2), H - 20);
    }
  }

  // 8×8 = 64 px 圓形燈泡；黑底、燈座、可選 glow。
  function drawMatrix(frame) {
    const cv = $("#modeMatrix");
    if (!cv) { return; }
    const ctx = cv.getContext("2d");
    const W = cv.width, H = cv.height;
    const cell = Math.min(W, H) / 8;
    const rad = Math.max(2, cell * 0.36);
    const glow = !!($("#modeGlow") && $("#modeGlow").checked);
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = "#05070a"; ctx.fillRect(0, 0, W, H);
    for (let row = 0; row < 8; row++) {
      for (let col = 0; col < 8; col++) {
        const px = (frame && frame[row * 8 + col]) || [0, 0, 0, 0];
        const [r, g, b] = displayRGB(px);
        const cx = (col + 0.5) * cell;
        const cy = (row + 0.5) * cell;
        // 燈座（暗圈）
        ctx.beginPath();
        ctx.arc(cx, cy, rad + Math.max(1.5, cell * 0.05), 0, Math.PI * 2);
        ctx.fillStyle = "#0c1016";
        ctx.fill();
        if (glow && (r + g + b) > 24) {
          ctx.save();
          ctx.shadowColor = "rgb(" + r + "," + g + "," + b + ")";
          ctx.shadowBlur = Math.max(4, cell * 0.34);
        }
        ctx.beginPath();
        ctx.arc(cx, cy, rad, 0, Math.PI * 2);
        ctx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
        ctx.fill();
        if (glow) { ctx.restore(); }
        // 高光（唔會蓋過顏色）
        const lum = Math.max(r, g, b) / 255;
        ctx.beginPath();
        ctx.arc(cx - rad * 0.30, cy - rad * 0.30, Math.max(1, rad * 0.26),
          0, Math.PI * 2);
        ctx.fillStyle = "rgba(255,255,255," + (0.04 + 0.22 * lum).toFixed(3) + ")";
        ctx.fill();
      }
    }
  }

  function draw() {
    const frame = displayFrame();
    drawStrip(frame);
    drawMatrix(frame);
    const chip = $("#modeFrameInfo");
    if (chip && !S.override) {
      const gain = ($("#modeGain") && $("#modeGain").checked) ? " · 顯示×8" : "";
      chip.textContent = S.cur ? ("模擬合成 · white " + S.white + "%" + gain) : "未載入";
    }
  }

  function renderInfo() {
    const info = $("#modeInfo");
    if (info && S.cur) {
      info.textContent = S.cur.name + " · " + S.entries.length + " 項 · " +
        S.totalF + " 步 · " + S.leds + " px";
    }
    const pre = $("#modeEntries");
    if (pre) {
      pre.textContent = (S.entries || []).map((en, i) =>
        (i + 1) + ". group=" + en.raw.group + "  effect=" + en.effectName +
        "  write=" + en.write + "  range=" + en.range +
        (en.groupObj ? "" : "  ⚠ 群組未找到") +
        (en.effect ? "" : "  ⚠ 效果未找到")).join("\n") || "—";
    }
  }

  function selectMode(i) {
    S.cur = S.modes[i] || null;
    S.step = 0;
    clearOverride();
    if (!S.cur) { return; }
    S.entries = buildEntries(S.cur);
    // 由 ranges 推燈鏈長度（有寫 range 就先跟最大值，否則用輸入值）
    let hint = 0;
    S.entries.forEach((en) => {
      const m = /^\s*(\d+)\s*:\s*(\d+)/.exec(String(en.range));
      if (m) { hint = Math.max(hint, parseInt(m[2], 10)); }
    });
    if (hint > 0) {
      S.leds = hint;
      const inp = $("#modeLeds");
      if (inp) { inp.value = String(hint); }
    }
    S.totalF = totalFrames(S.cur);
    S.entries.forEach((en) => { en.indices = sliceIndices(en.range, S.leds); });
    // 跟 mode JSON 的 white（firmware 也是這樣）；缺欄位 = 0，不要沿用上一個 mode 的 slider。
    const mw = (S.cur && S.cur.white !== undefined) ? Number(S.cur.white) : 0;
    S.white = Math.max(0, Math.min(100, Number.isFinite(mw) ? mw : 0));
    const whiteEl = $("#modeWhite");
    if (whiteEl) { whiteEl.value = String(S.white); }
    const whiteVal = $("#modeWhiteVal");
    if (whiteVal) { whiteVal.textContent = String(S.white); }
    renderInfo(); draw();
  }

  async function loadTree() {
    const base = ($("#modeBase") && $("#modeBase").value || "/pixel").replace(/\/+$/, "");
    try {
      // 先重建 manifest，確保新上傳／seed 的檔案出現在預覽（0x200B FILE_SCAN）。
      try { await fetch("/api/scan", { method: "POST" }); } catch (_) { /* 冇連線都照試 manifest */ }
      const mf = await (await fetch("/api/manifest")).json();
      const paths = Object.keys(mf.data || {});
      if (!paths.length) { throw new Error("manifest 空：先按『重建索引』"); }
      const effPath = paths.find((p) => p === base + "/effects/effects.json");
      if (!effPath) { throw new Error("搵唔到 " + base + "/effects/effects.json"); }
      const effDoc = await apiFile(effPath);
      S.effects = {};
      (effDoc.effects || []).forEach((e) => {
        const c = compileEffect(e);
        if (c) { S.effects[e.name] = c; }
      });
      S.maps = {};
      for (const p of paths.filter((x) => x.startsWith(base + "/map/") && x.endsWith(".json"))) {
        const m = await apiFile(p);
        S.maps[m.name] = m;
        S.maps[String(m.id)] = m;
      }
      S.modes = [];
      for (const p of paths.filter((x) => x.startsWith(base + "/modes/") && x.endsWith(".json"))) {
        S.modes.push(await apiFile(p));
      }
      S.modes.sort((a, b) => (a.index || 0) - (b.index || 0));
      const regPath = paths.find((p) => p === base + "/registry.json");
      S.registry = regPath ? await apiFile(regPath) : null;
      const regPre = $("#modeRegistry");
      if (regPre) {
        regPre.textContent = S.registry
          ? JSON.stringify(S.registry, null, 2)
          : "（冇 registry.json）";
      }
      const sel = $("#modeSelect");
      if (sel) {
        sel.innerHTML = "";
        S.modes.forEach((m, i) => {
          const o = document.createElement("option");
          o.value = String(i);
          o.textContent = (m.id ? m.id + ". " : "") + m.name +
            (m.mapping ? "  [" + m.mapping + "]" : "");
          sel.appendChild(o);
        });
        sel.onchange = () => selectMode(parseInt(sel.value, 10));
      }
      if (S.modes.length) { selectMode(0); }
      else { draw(); }
      const info = $("#modeInfo");
      if (info) {
        info.textContent = "已載入 " + Object.keys(S.effects).length + " 效果／" +
          S.modes.length + " 模式／" + Object.keys(S.maps).length + " mapping";
      }
      toast("pixel 樹已載入（" + base + "）", "ok");
      return true;
    } catch (e) {
      toast("載入失敗：" + e.message, "err", 6000);
      draw();
      return false;
    }
  }

  function tick() {
    if (!S.cur) { return; }
    clearOverride();          // 裝置 frame 只顯示到用戶再播為止
    S.step = (S.step + 1) % Math.max(1, S.totalF);
    draw();
  }

  function play(on) {
    if (S.timer) { clearInterval(S.timer); S.timer = null; }
    if (on) {
      clearOverride();
      S.timer = setInterval(tick, 20);
    }
    const b = $("#btnModePlay");
    if (b) { b.textContent = on ? "⏸ 暫停" : "▶ 播放"; }
  }

  // ── 讀取裝置實際 frame（0xF06B → 0xF06C）───────────────
  function parseDeviceFrame(reply) {
    let hex = "";
    let seq = null;
    let px = null;
    const payload = reply && reply.json;
    if (payload && typeof payload === "object") {
      if (typeof payload.hex === "string") { hex = payload.hex; }
      if (payload.seq !== undefined) { seq = payload.seq; }
      if (payload.px !== undefined) { px = Number(payload.px); }
      if (payload.mode !== undefined) { var modeName = String(payload.mode); }
      if (payload.white !== undefined) { var whitePct = Number(payload.white); }
      if (payload.apply_us !== undefined) { var applyUs = Number(payload.apply_us); }
      if (!hex && payload.frame && typeof payload.frame.hex === "string") {
        hex = payload.frame.hex;
        if (payload.frame.seq !== undefined) { seq = payload.frame.seq; }
      }
    }
    if (!hex && reply && typeof reply.hex === "string") { hex = reply.hex; }
    if (!hex) { throw new Error("回覆冇 frame hex（firmware 未支援 0xF06B？）"); }
    const bytes = hexToBytes(hex);              // 00-core.js
    if (!bytes.length || bytes.length % 4 !== 0) {
      throw new Error("frame hex 長度唔係 4 B/px：" + bytes.length + " B");
    }
    const leds = Math.min(64, Math.floor(bytes.length / 4));
    if (px && px !== leds && px !== 64) { /* 只作參考 */ }
    const frame = [];
    for (let i = 0; i < 64; i++) {
      if (i < leds) {
        frame.push([bytes[i * 4], bytes[i * 4 + 1], bytes[i * 4 + 2], bytes[i * 4 + 3]]);
      } else {
        frame.push([0, 0, 0, 0]);
      }
    }
    return { frame: frame, seq: seq, px: leds, mode: (typeof modeName === "string" ? modeName : null),
             white: (typeof whitePct === "number" ? whitePct : null),
             applyUs: (typeof applyUs === "number" ? applyUs : null) };
  }

  // 把目前選中的 mode 套用到裝置（0xF06D → 0xF06E）；下一幀邊界 rebuild plan。
  async function applyModeToDevice() {
    if (!S.cur || S.cur.id === undefined) {
      toast("先揀一個 mode", "err", 4000);
      return;
    }
    const id = Number(S.cur.id) & 0xFF;
    try {
      const reply = await api("/api/cmd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          cmd: "0xF06D", payload_hex: id.toString(16).padStart(2, "0"),
          expect: ["0xF06E"], timeout: 5,
        }),
      });
      toast("已排隊切 mode " + id + "（" + (S.cur.name || "?") + "）", "ok", 4000);
      setTimeout(() => readDeviceFrame(true), 350);
      return reply;
    } catch (e) {
      toast("切 mode 失敗：" + e.message + "（firmware 要支援 0xF06D）", "err", 9000);
      return null;
    }
  }

  // 0xF06F：重新掃 /pixel 四層，下一幀邊界 reload（改檔不用重開）。
  async function reloadDeviceContent() {
    try {
      await api("/api/cmd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          cmd: "0xF06F", payload_hex: "", expect: ["0xF070"], timeout: 5,
        }),
      });
      toast("已排隊重新載入 /pixel（下一幀邊界）", "ok", 4000);
      setTimeout(() => { loadTree(); readDeviceFrame(true); }, 450);
      return true;
    } catch (e) {
      toast("重新載入失敗：" + e.message + "（firmware 要支援 0xF06F）", "err", 9000);
      return false;
    }
  }

  async function readDeviceFrame(quiet) {
    const chip = $("#modeFrameInfo");
    try {
      const reply = await api("/api/cmd", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({
          cmd: "0xF06B", payload_hex: "", expect: ["0xF06C"], timeout: 5,
        }),
      });
      const parsed = parseDeviceFrame(reply);
      S.override = { frame: parsed.frame, seq: parsed.seq, at: Date.now() };
      play(false);
      draw();
      if (chip) {
        chip.textContent = "裝置 frame"
          + (parsed.seq !== null && parsed.seq !== undefined ? " · seq " + parsed.seq : "")
          + " · " + parsed.px + " px"
          + (parsed.mode ? " · " + parsed.mode : "")
          + (parsed.white !== null ? " · white " + parsed.white + "%" : "")
          + (parsed.applyUs !== null ? " · apply " + parsed.applyUs + " µs" : "");
      }
      if (!quiet) toast("已讀取裝置 frame（" + parsed.px + " px）", "ok", 4000);
      return parsed.frame;
    } catch (e) {
      if (chip) { chip.textContent = "讀取失敗"; }
      if (!quiet) {
        toast("讀取裝置 frame 失敗：" + e.message
          + "（firmware 要支援 0xF06B SHOWFRAME；未支援唔會影響其他功能）", "err", 9000);
      }
      return null;                     // 明確唔 crash
    }
  }

  // ── Show 全流程卡 ─────────────────────────────────────
  function flowSet(text, tone) {
    const chip = $("#flowState");
    if (!chip) { return; }
    chip.textContent = text;
    chip.className = "chip" + (tone ? " " + tone : "");
  }

  function flowLog(text) {
    const pre = $("#flowLog");
    if (!pre) { return; }
    const old = pre.textContent === "—" ? "" : pre.textContent + "\n";
    pre.textContent = old + text;
    pre.scrollTop = pre.scrollHeight;
  }

  function flowReset() {
    S.override = null;
    const pre = $("#flowLog");
    if (pre) { pre.textContent = "—"; }
    flowSet("待開始", "");
    draw();
  }

  async function flowLoad() {
    flowSet("載入 pixel 樹…", "amber");
    flowLog("→ 載入 /pixel（manifest → effects/map/modes/registry）");
    const ok = await loadTree();
    if (ok) {
      flowLog("✔ 已載入：效果 " + Object.keys(S.effects).length
        + "／模式 " + S.modes.length + "／mapping " + Object.keys(S.maps).length);
      flowSet("已載入", "led");
    } else {
      flowLog("✘ 載入失敗（睇上面 toast／確認 /pixel 檔案已上傳）");
      flowSet("載入失敗", "err");
    }
  }

  async function flowPreview() {
    if (!S.cur) {
      await flowLoad();
      if (!S.cur) { return; }
    }
    draw();
    flowLog("✔ 8×8 預覽已更新（64 px，white " + S.white + "%）");
    flowSet("預覽中", "led");
  }

  async function flowBuild() {
    if (typeof BuildApi === "undefined") {
      flowLog("✘ build 模組未載入（61-build.js）");
      flowSet("Build 失敗", "err");
      return;
    }
    flowSet("Build 中…", "amber");
    flowLog("→ POST /api/build（用固件頁 Build 卡揀的板／project）");
    try {
      const r = await BuildApi.build();
      if (r && r.ok) {
        flowLog("✔ Build 完成 → " + (r.artifact || "（冇產物）")
          + " · " + r.size + " B · " + r.elapsed + "s");
        flowSet("Build 完成", "led");
        flowLog("下一步：按 4 · OTA（或去固件頁確認）");
        const otaBtn = $("#btnFlowOta");
        if (otaBtn) { otaBtn.disabled = !r.artifact; }
      } else {
        flowLog("✘ Build 失敗 rc=" + (r ? r.returncode : "?"));
        flowSet("Build 失敗", "err");
      }
    } catch (e) {
      flowLog("✘ Build 失敗：" + e.message);
      flowSet("Build 失敗", "err");
    }
  }

  async function flowOta() {
    if (typeof BuildApi === "undefined") {
      flowLog("✘ build 模組未載入（61-build.js）");
      return;
    }
    flowSet("OTA 中…", "amber");
    try {
      const last = BuildApi.last();
      flowLog("→ OTA " + (last && last.artifact ? last.artifact : "（未 build）"));
      const r = await BuildApi.ota();
      if (r) {
        flowLog("✔ OTA 完成：" + fmtBytes((r.data && r.data.size) || 0));
        flowSet("OTA 完成", "led");
      } else {
        flowLog("（已取消）");
        flowSet("已取消", "");
      }
    } catch (e) {
      flowLog("✘ OTA 失敗：" + e.message);
      flowSet("OTA 失敗", "err");
    }
  }

  async function flowRead() {
    flowSet("讀取 frame…", "amber");
    const frame = await readDeviceFrame(true);
    if (frame) {
      flowLog("✔ 讀到 device frame（" + Math.min(64, frame.length) + " px）");
      flowSet("已讀 device frame", "led");
    } else {
      flowLog("✘ 讀唔到 device frame（firmware 未支援 0xF06B？）");
      flowSet("讀取失敗", "err");
    }
  }

  // ── 綁定 ──────────────────────────────────────────────
  function bind() {
    const load = $("#btnModeLoad");
    if (load) { load.onclick = loadTree; }
    const b = $("#btnModePlay");
    if (b) { b.onclick = () => play(!S.timer); }
    const z = $("#btnModeZero");
    if (z) { z.onclick = () => { S.step = 0; draw(); }; }
    const leds = $("#modeLeds");
    if (leds) {
      leds.onchange = () => {
        S.leds = Math.max(1, parseInt(leds.value, 10) || 64);
        S.entries.forEach((en) => { en.indices = sliceIndices(en.range, S.leds); });
        renderInfo(); draw();
      };
    }
    // white mix slider
    const white = $("#modeWhite");
    const whiteVal = $("#modeWhiteVal");
    if (white) {
      S.white = Math.max(0, Math.min(100, parseInt(white.value, 10) || 0));
      if (whiteVal) { whiteVal.textContent = String(S.white); }
      white.oninput = () => {
        S.white = Math.max(0, Math.min(100, parseInt(white.value, 10) || 0));
        if (whiteVal) { whiteVal.textContent = String(S.white); }
        clearOverride();               // 改白比例就係想睇合成結果
        draw();
      };
    }
    const glow = $("#modeGlow");
    if (glow) { glow.onchange = draw; }
    const gain = $("#modeGain");
    if (gain) { gain.onchange = draw; }
    const matrixZero = $("#btnModeMatrixZero");
    if (matrixZero) {
      matrixZero.onclick = () => {
        S.override = null;
        const frame = [];
        for (let i = 0; i < 64; i++) { frame.push([0, 0, 0, 0]); }
        drawMatrix(frame);
        const chip = $("#modeFrameInfo");
        if (chip) { chip.textContent = "已清空"; }
      };
    }
    const applyBtn = $("#btnModeApply");
    if (applyBtn) { applyBtn.onclick = () => { applyModeToDevice(); }; }
    const reloadBtn = $("#btnModeReload");
    if (reloadBtn) { reloadBtn.onclick = () => { reloadDeviceContent(); }; }
    const readBtn = $("#btnModeRead");
    if (readBtn) { readBtn.onclick = () => { readDeviceFrame(); }; }
    // Show 全流程
    const flowLoadBtn = $("#btnFlowLoad");
    if (flowLoadBtn) { flowLoadBtn.onclick = () => { flowLoad(); }; }
    const flowPrevBtn = $("#btnFlowPreview");
    if (flowPrevBtn) { flowPrevBtn.onclick = () => { flowPreview(); }; }
    const flowBuildBtn = $("#btnFlowBuild");
    if (flowBuildBtn) { flowBuildBtn.onclick = () => { flowBuild(); }; }
    const flowOtaBtn = $("#btnFlowOta");
    if (flowOtaBtn) { flowOtaBtn.onclick = () => { flowOta(); }; }
    const flowReadBtn = $("#btnFlowRead");
    if (flowReadBtn) { flowReadBtn.onclick = () => { flowRead(); }; }
    const flowResetBtn = $("#btnFlowReset");
    if (flowResetBtn) { flowResetBtn.onclick = flowReset; }
    draw();                              // 未載入都畫個黑底 8×8
  }

  function onShow() {
    draw();
    if (typeof BuildApi !== "undefined" && BuildApi.refreshButtons) {
      BuildApi.refreshButtons();
    }
  }

  document.addEventListener("DOMContentLoaded", () => setTimeout(bind, 0));

  window.__ModeSimState = S;
  return { loadTree: loadTree, play: play, onShow: onShow, draw: draw };
})();
