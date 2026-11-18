/* ESP WebMaster — NCM 容器檢視器（.nct／.nce／.nca）
 * 檔頭／manifest／meta／requires-provides／CRC 的呈現。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module），
 * 所以唔需要 build step，直接用 file／http 都跑得。
 */
"use strict";

// ── NCM 容器（.nct / .nce / .nca）檢視 ─────────────────────
function isContainerPath(path) {
  return /\.(nct|nce|nca)$/i.test(String(path || ""));
}

async function fetchContainer(path) {
  const r = await fetch(`/api/container?path=${encodeURIComponent(path)}`);
  if (!r.ok) {
    let msg = `${r.status}`;
    try { const j = await r.json(); msg = JSON.stringify(j.detail ?? j); } catch (_) { /* */ }
    throw new Error(msg);
  }
  const j = await r.json();
  if (!j.ok) throw new Error(JSON.stringify(j));
  return j.data;
}

function _el(tag, cls, text) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined && text !== null) e.textContent = String(text);
  return e;
}

function _kvTable(rows) {
  const t = _el("table", "ncm-kv");
  for (const [k, v] of rows) {
    if (v === undefined || v === null || v === "") continue;
    const tr = _el("tr");
    tr.appendChild(_el("th", "", k));
    tr.appendChild(_el("td", "", v));
    t.appendChild(tr);
  }
  return t;
}

function ideRenderContainer(c) {
  const box = $("#ideContainer");
  if (!box) return;
  box.innerHTML = "";
  if (!c) { box.appendChild(_el("p", "muted", "冇資料")); return; }

  const head = _el("div", "ncm-head");
  head.appendChild(_el("span", "ncm-badge " + (c.valid ? "ok" : "bad"),
                       c.valid ? "✓ 容器可以載入" : "✗ 有問題"));
  const kind = c.manifest ? (c.manifest.kind_name || "?") : "?";
  head.appendChild(_el("span", "ncm-badge", "kind: " + kind));
  if (c.header) head.appendChild(_el("span", "ncm-badge", "target: " + c.header.target_name));
  head.appendChild(_el("span", "grow"));
  box.appendChild(head);

  const title = c.meta && c.meta.name ? c.meta.name : c.file.name;
  box.appendChild(_el("h2", "ncm-title", title));
  if (c.meta && c.meta.desc) box.appendChild(_el("p", "ncm-desc", c.meta.desc));
  if (c.meta && c.meta.usage) {
    box.appendChild(_el("div", "ncm-label", "用法"));
    box.appendChild(_el("pre", "ncm-usage", c.meta.usage));
  }

  const errs = [...(c.errors || []), ...(c.warnings || [])];
  if (errs.length) {
    const ul = _el("ul", "ncm-issues");
    for (const x of c.errors || []) ul.appendChild(_el("li", "bad", "✗ " + x));
    for (const x of c.warnings || []) ul.appendChild(_el("li", "warn", "⚠ " + x));
    box.appendChild(ul);
  }

  // 檔案／檔頭
  const h = c.header || {};
  const m = c.manifest || {};
  const p = c.payload || {};
  box.appendChild(_el("h3", "ncm-h3", "檔案／檔頭"));
  box.appendChild(_kvTable([
    ["檔名", `${c.file.name}（${fmtBytes(c.file.size)}）`],
    ["副檔名", c.file.ext + (c.file.ext_known ? "" : "（非 .nct／.nce）")],
    ["magic", h.magic],
    ["ABI", h.abi],
    ["flags", `${h.flags}${h.flags_list && h.flags_list.length ? "（" + h.flags_list.join(" / ") + "）" : ""}`],
    ["payload", `off ${p.offset}／size ${fmtBytes(p.size)}／${p.format}${p.aot ? "（AOT）" : ""}`],
    ["payload CRC", `${p.crc_expected} → ${p.crc_actual}（${p.crc_ok ? "OK" : "不符"}）`],
    ["manifest", c.manifest ? `off ${h.manifest_off}／${h.manifest_size} B（ver ${m.version}）` : "冇"],
    ["meta", c.meta ? "有（name／desc／usage／requires／provides）" : "冇"],
  ]));

  // 載入參數
  if (c.manifest) {
    box.appendChild(_el("h3", "ncm-h3", "載入參數（manifest）"));
    box.appendChild(_kvTable([
      ["kind", `${m.kind_name}（${m.kind}）`],
      ["target", h.target_name],
      ["core", m.core < 0 ? "不限（-1）" : m.core],
      ["layer", m.layer],
      ["prio", m.prio],
      ["fps", m.fps],
      ["px", m.px],
      ["mem（線性記憶體）", fmtBytes(m.mem)],
      ["stack", fmtBytes(m.stack)],
      ["budget_us", `${m.budget_us} µs／tick`],
      ["指令數", m.ncmds],
      ["manifest flags / reserved", `${m.flags} / ${m.reserved}`],
    ]));
    if (m.cmds && m.cmds.length) {
      box.appendChild(_el("div", "ncm-label", "宣告指令（外掛私有 cmd）"));
      box.appendChild(_el("div", "ncm-cmds",
        m.cmds.map((x) => "0x" + x.toString(16).toUpperCase().padStart(4, "0")).join("  ")));
    }
  }

  // meta：requires / provides
  if (c.meta) {
    const reqs = c.meta.requires || [];
    const provs = c.meta.provides || [];
    for (const [label, list] of [["需要資源（requires）", reqs], ["提供資源（provides）", provs]]) {
      box.appendChild(_el("h3", "ncm-h3", label));
      if (!list.length) {
        box.appendChild(_el("p", "muted", "冇宣告"));
        continue;
      }
      const t = _el("table", "ncm-kv");
      const trh = _el("tr");
      for (const x of ["type", "name", "bytes", "exclusive"]) trh.appendChild(_el("th", "", x));
      t.appendChild(trh);
      for (const e of list) {
        const tr = _el("tr");
        tr.appendChild(_el("td", "", e.type_name || e.type));
        tr.appendChild(_el("td", "mono", e.name || ""));
        tr.appendChild(_el("td", "", e.bytes || ""));
        tr.appendChild(_el("td", "", e.exclusive ? "是" : ""));
        t.appendChild(tr);
      }
      box.appendChild(t);
    }
  }

  // 原始檔頭
  if (c.hex) {
    box.appendChild(_el("h3", "ncm-h3", "原始位元組（前 96 B）"));
    box.appendChild(_el("pre", "ncm-hex", c.hex));
  }
}
