/* ESP WebMaster — 模式預覽：registry → mode → map → effect 四層合成
 *
 * 為什麼要另一頁：`effects.json` 只係第一層（怎麼動）。真正上燈睇到嘅係
 *   registry.list（播邊個 mode）→ mode.map[]（group × effect × write × range）
 *   → map/*.json 的 groups[].sel（邊啲燈）→ effects.json 的 program（波形）
 * 呢頁照 pixel_task 的 scatter 語義合成：單通道寫 r/g/b/w 讀 vals[k]，
 * rgb 讀 vals[3k..3k+2]，rgbw 讀 vals[4k..]（同 pixel_layout._scatter_* 一致）。
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

  function draw() {
    const cv = document.querySelector("#modeStrip");
    if (!cv) { return; }
    const ctx = cv.getContext("2d");
    const W = cv.width, H = cv.height;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = "#0b0d10"; ctx.fillRect(0, 0, W, H);
    if (!S.cur) { return; }
    const out = composeFrame(S.step);
    const cw = W / S.leds;
    for (let i = 0; i < S.leds; i++) {
      const p = out[i];
      const r = Math.min(255, p[0] + p[3]);
      const g = Math.min(255, p[1] + p[3]);
      const b = Math.min(255, p[2] + p[3]);
      ctx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
      ctx.fillRect(Math.floor(i * cw) + 1, 10, Math.max(1, Math.floor(cw) - 2), H - 20);
    }
  }

  function renderInfo() {
    const info = document.querySelector("#modeInfo");
    if (info && S.cur) {
      info.textContent = S.cur.name + " · " + S.entries.length + " 項 · " +
        S.totalF + " 步 · " + S.leds + " px";
    }
    const pre = document.querySelector("#modeEntries");
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
      const inp = document.querySelector("#modeLeds");
      if (inp) { inp.value = String(hint); }
    }
    S.totalF = totalFrames(S.cur);
    S.entries.forEach((en) => { en.indices = sliceIndices(en.range, S.leds); });
    renderInfo(); draw();
  }

  async function loadTree() {
    const base = (document.querySelector("#modeBase").value || "/pixel").replace(/\/+$/, "");
    try {
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
      const regPre = document.querySelector("#modeRegistry");
      if (regPre) {
        regPre.textContent = S.registry
          ? JSON.stringify(S.registry, null, 2)
          : "（冇 registry.json）";
      }
      const sel = document.querySelector("#modeSelect");
      sel.innerHTML = "";
      S.modes.forEach((m, i) => {
        const o = document.createElement("option");
        o.value = String(i);
        o.textContent = (m.id ? m.id + ". " : "") + m.name +
          (m.mapping ? "  [" + m.mapping + "]" : "");
        sel.appendChild(o);
      });
      sel.onchange = () => selectMode(parseInt(sel.value, 10));
      if (S.modes.length) { selectMode(0); }
      const info = document.querySelector("#modeInfo");
      if (info) { info.textContent = "已載入 " + Object.keys(S.effects).length + " 效果／" +
        S.modes.length + " 模式／" + Object.keys(S.maps).length + " mapping"; }
      toast("pixel 樹已載入（" + base + "）", "ok");
    } catch (e) {
      toast("載入失敗：" + e.message, "err", 6000);
    }
  }

  function tick() {
    if (!S.cur) { return; }
    S.step = (S.step + 1) % Math.max(1, S.totalF);
    draw();
  }

  function play(on) {
    if (S.timer) { clearInterval(S.timer); S.timer = null; }
    if (on) { S.timer = setInterval(tick, 20); }
    const b = document.querySelector("#btnModePlay");
    if (b) { b.textContent = on ? "⏸ 暫停" : "▶ 播放"; }
  }

  function bind() {
    const load = document.querySelector("#btnModeLoad");
    if (load) { load.onclick = loadTree; }
    const b = document.querySelector("#btnModePlay");
    if (b) { b.onclick = () => play(!S.timer); }
    const z = document.querySelector("#btnModeZero");
    if (z) { z.onclick = () => { S.step = 0; draw(); }; }
    const leds = document.querySelector("#modeLeds");
    if (leds) {
      leds.onchange = () => {
        S.leds = Math.max(1, parseInt(leds.value, 10) || 64);
        S.entries.forEach((en) => { en.indices = sliceIndices(en.range, S.leds); });
        renderInfo(); draw();
      };
    }
  }

  document.addEventListener("DOMContentLoaded", () => setTimeout(bind, 0));

  return { loadTree: loadTree, play: play };
})();
