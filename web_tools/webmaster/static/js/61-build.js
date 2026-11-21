/* ESP WebMaster — 建置（repo 根 make.py）＋一鍵 OTA
 *
 * 為什麼由 server 起 build：板子真相喺 `ports/`、project 喺 `ports/project/`，
 * 只有 repo 根 `make.py build` 識得砌命令。瀏覽器冇檔案系統，所以只可以叫
 * server 跑（POST /api/build），再將 stdout/stderr 顯示出嚟。
 *
 * 一鍵 OTA：GET /api/build/artifact 拿 blob → POST 現有 /api/firmware（binary body），
 * 唔會另開一條燒錄路徑。
 *
 * 載入次序由 static/index.html 的 <script> 決定；全部係傳統 script（唔用 module）。
 */
"use strict";

const BuildApi = (function () {
  const S = {
    info: null,       // GET /api/build/info 的 data
    last: null,       // 最近一次 POST /api/build 的回覆
    busy: false,
    loading: null,    // in-flight loadInfo promise（避免重複請求）
  };

  function setChip(sel, text, tone) {
    const node = $(sel);
    if (!node) return;
    node.textContent = text;
    node.className = "chip" + (tone ? " " + tone : "");
  }

  function fmtSeconds(n) {
    const v = Number(n) || 0;
    return v >= 60 ? (v / 60).toFixed(1) + " min" : v.toFixed(1) + " s";
  }

  function boardInfo(name) {
    return ((S.info && S.info.boards) || []).find((b) => b.name === name) || null;
  }

  function projectOptions() {
    const board = $("#buildBoard");
    const b = boardInfo(board ? board.value : "");
    return (b && b.projects) || [];
  }

  // ── 下拉選單 ─────────────────────────────────────────
  function fillProjects(keep) {
    const sel = $("#buildProject");
    if (!sel) return;
    const want = keep !== undefined ? keep : sel.value;
    sel.innerHTML = "";
    const auto = document.createElement("option");
    auto.value = "";
    auto.textContent = "（default project）";
    sel.appendChild(auto);
    projectOptions().forEach((p) => {
      const o = document.createElement("option");
      o.value = p.name;
      o.textContent = p.name + (p.default ? "（default）" : "");
      sel.appendChild(o);
    });
    if (want && projectOptions().some((p) => p.name === want)) sel.value = want;
  }

  function fillBoards() {
    const sel = $("#buildBoard");
    if (!sel || !S.info) return;
    const keep = sel.value;
    sel.innerHTML = "";
    ((S.info && S.info.boards) || []).forEach((b) => {
      const o = document.createElement("option");
      o.value = b.name;
      o.textContent = b.name + (b.chip ? "  [" + b.chip + "]" : "");
      sel.appendChild(o);
    });
    if (keep && (S.info.boards || []).some((b) => b.name === keep)) {
      sel.value = keep;
    } else {
      // 預設揀 S3 Matrix（8×8 / 64 px：mode 模擬器同 Show 流程都係佢）
      const prefer = (S.info.boards || []).find((b) => /S3-Matrix/.test(b.name))
        || (S.info.boards || [])[0];
      if (prefer) sel.value = prefer.name;
    }
    sel.onchange = () => fillProjects("");
    fillProjects();
  }

  function renderInfo() {
    if (!S.info) return;
    setChip("#buildIdf", S.info.idfPath ? "IDF " + S.info.idfPath : "IDF 未找到",
      S.info.idfPath ? "led" : "amber");
    const idf = $("#buildIdf");
    if (idf) idf.title = S.info.idfPath || "（IDF_PATH／~/esp/esp-idf／<repo>/.idf/esp-idf）";
    if (S.info.makePyExists && S.info.idfPath) {
      setChip("#buildState", "就緒（" + ((S.info.boards || []).length) + " 塊板）", "led");
    } else {
      setChip("#buildState", S.info.makePyExists ? "IDF 未找到" : "make.py 缺失", "amber");
    }
  }

  // ── GET /api/build/info ──────────────────────────────
  function loadInfo(quiet) {
    if (S.loading) return S.loading;
    S.loading = (async () => {
      const st = $("#buildState");
      if (st) st.textContent = "讀取環境…";
      try {
        const r = await api("/api/build/info");
        S.info = r.data || {};
        fillBoards();
        renderInfo();
        return S.info;
      } catch (e) {
        setChip("#buildState", "讀唔到 build 環境", "amber");
        if (!quiet) toast("讀 /api/build/info 失敗：" + e.message, "err", 8000);
        return null;
      } finally {
        S.loading = null;
      }
    })();
    return S.loading;
  }

  function selectedOptions() {
    const boardSel = $("#buildBoard");
    const projSel = $("#buildProject");
    return {
      board: boardSel ? boardSel.value : "",
      project: projSel ? projSel.value : "",
      tests: !!($("#buildTests") && $("#buildTests").checked),
      fresh: !!($("#buildFresh") && $("#buildFresh").checked),
    };
  }

  function renderLog(lines, max) {
    const pre = $("#buildLog");
    if (!pre) return;
    const limit = max || 1200;
    const shown = lines.length > limit ? lines.slice(-limit) : lines;
    pre.textContent = (lines.length > limit
      ? "（只顯示最後 " + limit + " 行，共 " + lines.length + " 行）\n" : "")
      + shown.join("\n");
  }

  function renderResult(r) {
    if (!r) return;
    S.last = r;
    renderLog(r.lines || []);
    const art = $("#buildArtifact");
    if (art) {
      art.textContent = r.artifact ? (r.artifact + " · " + fmtBytes(r.size)) : "冇產物";
      art.title = r.artifact || "";
    }
    setChip("#buildState",
      r.ok ? "建置成功（" + fmtSeconds(r.elapsed) + "）" : "建置失敗（rc=" + r.returncode + "）",
      r.ok ? "led" : "amber");
    refreshButtons();
  }

  function refreshButtons() {
    const ota = $("#btnBuildOta");
    if (ota) ota.disabled = S.busy || !(S.last && S.last.ok && S.last.artifact);
    const flowOta = $("#btnFlowOta");
    if (flowOta) flowOta.disabled = !(S.last && S.last.ok && S.last.artifact);
    const build = $("#btnBuild");
    if (build) build.disabled = S.busy;
  }

  // ── POST /api/build ──────────────────────────────────
  async function build(opts) {
    opts = opts || {};
    if (S.busy) throw new Error("已經有 build 喺跑");
    if (!S.info) await loadInfo(true);
    const sel = selectedOptions();
    const board = opts.board || sel.board;
    if (!board) throw new Error("先揀板（或按 ⟳ 重新讀環境）");
    const project = opts.project !== undefined ? (opts.project || "") : sel.project;
    const tests = opts.tests !== undefined ? !!opts.tests : sel.tests;
    const fresh = opts.fresh !== undefined ? !!opts.fresh : sel.fresh;

    S.busy = true;
    S.last = null;
    refreshButtons();
    setChip("#buildState", "建置中…（最長 600s）", "amber");
    const pre = $("#buildLog");
    if (pre) {
      pre.textContent = "→ python make.py build " + board
        + (project ? " --project " + project : "")
        + (tests ? " --tests" : "") + (fresh ? " --fresh" : "")
        + "\n（IDF build 通常 30–180 秒；--fresh 會再慢少少）";
    }
    const body = { board: board, project: project || null, tests: tests, fresh: fresh };
    try {
      const r = await api("/api/build", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      renderResult(r);
      if (r.ok) {
        toast("建置完成：" + (r.artifact || "（冇 archive）")
          + " · " + fmtSeconds(r.elapsed), "ok", 6000);
        logLine("BUILD 完成：" + board + (project ? " / " + project : "")
          + (r.artifact ? " → " + r.artifact : ""), "info");
      } else {
        toast("建置失敗（rc=" + r.returncode + "），睇固件頁 Build 卡的 log", "err", 9000);
        logLine("BUILD 失敗（rc=" + r.returncode + "）", "err");
      }
      return r;
    } catch (e) {
      setChip("#buildState", "建置失敗", "err");
      if (pre) pre.textContent = "✘ " + e.message;
      toast("建置失敗：" + e.message, "err", 9000);
      refreshButtons();
      throw e;
    } finally {
      S.busy = false;
      refreshButtons();
    }
  }

  // ── 一鍵 OTA（現有 /api/firmware）────────────────────
  async function ota(opts) {
    opts = opts || {};
    const result = opts.build || S.last;
    if (!result || !result.artifact) {
      throw new Error("未有建好的 artifact：先按 Build");
    }
    if (!state.connected) {
      throw new Error("未連線：先去上面揀 port，或按『重新連線』");
    }
    const apply = opts.apply !== undefined
      ? !!opts.apply : !!($("#fwApply") && $("#fwApply").checked);
    const target = state.port || "目前裝置";
    if (!confirm("用剛建好的 image OTA 到 " + target + "？\n\n"
      + result.artifact + "（" + fmtBytes(result.size) + "）\n\n"
      + "OTA 期間唔好拔線。")) {
      return null;
    }
    const url = "/api/build/artifact?path=" + encodeURIComponent(result.artifact);
    const resp = await fetch(url);
    if (!resp.ok) throw new Error("讀 artifact 失敗：HTTP " + resp.status);
    const blob = await resp.blob();
    const qs = new URLSearchParams({ apply: apply ? "1" : "0" });
    if (result.chip) qs.set("expect_chip", result.chip);
    if (opts.progress) opts.progress(0, blob.size);
    const up = await api("/api/firmware?" + qs.toString(), {
      method: "POST",
      headers: { "Content-Type": "application/octet-stream" },
      body: blob,
    });
    const data = (up && up.data) || {};
    toast("OTA 完成：" + fmtBytes(data.size || blob.size)
      + " · " + (data.seconds || "?") + "s", "ok", 7000);
    logLine("OTA 完成：" + result.artifact + " → " + target, "ok");
    return up;
  }

  // ── 綁定 ────────────────────────────────────────────
  function bind() {
    const reload = $("#btnBuildReload");
    if (reload) reload.onclick = () => { loadInfo(); };
    const buildBtn = $("#btnBuild");
    if (buildBtn) buildBtn.onclick = () => { build().catch(() => {}); };
    const otaBtn = $("#btnBuildOta");
    if (otaBtn) {
      otaBtn.onclick = () => {
        ota().catch((e) => toast("OTA 失敗：" + e.message, "err", 9000));
      };
    }
    refreshButtons();
    loadInfo(true);
  }

  document.addEventListener("DOMContentLoaded", () => setTimeout(bind, 0));

  return {
    loadInfo: loadInfo,
    build: build,
    ota: ota,
    refreshButtons: refreshButtons,
    info: () => S.info,
    last: () => S.last,
    busy: () => S.busy,
  };
})();
