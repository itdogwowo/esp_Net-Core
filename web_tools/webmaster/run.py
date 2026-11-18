#!/usr/bin/env python3
"""ESP WebMaster 啟動入口。

基礎數值（port／host／預設連邊個裝置／esptool 預設）全部喺設定檔：
**`web_tools/settings.json`**（同啟動器同層，團隊共用，入 git）＋
**`web_tools/settings.local.json`**（只係我部機，唔入 git）。
優先次序：**命令列 > settings.local.json > settings.json > 程式內建預設池**
（settings.json 可以只寫想改的 key，其餘落返預設）。

    python run.py                        # 跟設定檔（預設 8010、開瀏覽器）
    python run.py --sim                  # 模擬裝置（唔使板）
    python run.py --serial COM13         # 直接連 CDC（P4 OTG / S3 USB）
    python run.py --port 8011            # 換 port（永遠贏設定檔）
    python run.py --print-config         # 睇合併後的實際值 + 邊個檔提供
    python run.py --write-config         # 寫一份帶註解的 settings.json
"""
from __future__ import annotations

import argparse
import json
import sys
import threading
import time
import webbrowser

import uvicorn

import console
import env
import link
import settings as cfg
from server import create_app

# ★ 必須喺任何 print 之前：Windows 主控台係 cp950，遇到 Big5 冇的字（喺／嘅／搵）
#   會 UnicodeEncodeError 整個程式收場。同 make.py 一樣只放寬 errors，唔改 encoding。
console.setup()


def _build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="ESP WebMaster（設定檔：web_tools/settings.json / settings.local.json）")
    ap.add_argument("--host", default=None, help="綁定位址（預設跟設定檔 127.0.0.1）")
    ap.add_argument("--port", type=int, default=None,
                    help="HTTP port（預設跟設定檔 8010）")
    ap.add_argument("--serial", default=None, metavar="COMx",
                    help="啟動即連線的序列埠（覆寫 device.autoconnect）")
    ap.add_argument("--sim", action="store_true", help="用模擬裝置（虛擬 FAT）")
    ap.add_argument("--sim-dir", default=None, help="模擬裝置的根資料夾")
    ap.add_argument("--open", dest="open_browser", action="store_true", default=None,
                    help="啟動後開瀏覽器（預設跟設定檔）")
    ap.add_argument("--no-open", dest="open_browser", action="store_false",
                    help="唔開瀏覽器")
    ap.add_argument("--auto-port", dest="auto_port", action="store_true", default=None,
                    help="port 被佔用時自動揀下一個（預設跟設定檔）")
    ap.add_argument("--no-auto-port", dest="auto_port", action="store_false",
                    help="port 被佔用時直接報錯，唔自動換")
    ap.add_argument("--log-level", default=None,
                    choices=["critical", "error", "warning", "info", "debug", "trace"],
                    help="uvicorn 記錄等級（預設跟設定檔）")
    ap.add_argument("--no-env-check", action="store_true",
                    help="開機唔做環境自檢（唔會影響任何功能）")
    ap.add_argument("--list-ports", action="store_true",
                    help="只掃描序列埠並講解偵測結果（唔會起 server）")
    ap.add_argument("--print-config", action="store_true",
                    help="印出合併後的設定（含來源）然後結束")
    ap.add_argument("--write-config", action="store_true",
                    help="寫一份預設 settings.json 然後結束")
    ap.add_argument("--force", action="store_true", help="配合 --write-config：覆蓋已有的檔")
    return ap


def main(argv: list[str] | None = None) -> int:
    ap = _build_parser()
    args = ap.parse_args(argv)

    if args.write_config:
        path = cfg.write_default(force=args.force)
        print(f"[esp_webmaster] 設定檔：{path}")
        return 0

    try:
        conf, used = cfg.load(quiet=True)
    except cfg.SettingsError as e:
        print(f"[esp_webmaster] 設定檔有問題：{e}", file=sys.stderr)
        print("[esp_webmaster] 用 --print-config 睇實際值，或者 --write-config --force 重設。",
              file=sys.stderr)
        return 2

    srv, dev, fl = conf["server"], conf["device"], conf["flash"]

    if args.print_config:
        print(json.dumps({"used": used or ["內建預設（冇設定檔）"],
                          "effective": cfg.public(conf)},
                         ensure_ascii=False, indent=2))
        return 0

    host = args.host or srv["host"]
    port = args.port if args.port is not None else srv["port"]
    auto_port = srv["auto_port"] if args.auto_port is None else args.auto_port
    open_browser = srv["open_browser"] if args.open_browser is None else args.open_browser
    log_level = args.log_level or srv["log_level"]

    if cfg.port_in_use(host, port):
        if auto_port:
            found = cfg.find_free_port(host, port + 1, srv["auto_port_scan"])
            if not found:
                print(f"[esp_webmaster] Port {port} 已被佔用，而且自動掃描 {srv['auto_port_scan']} "
                      f"個都搵唔到空位；用 --port 換一個。", file=sys.stderr)
                return 6
            print(f"[esp_webmaster] Port {port} 已被佔用 → 改用 {found}")
            port = found
        else:
            print(f"[esp_webmaster] Port {port} 已被佔用（可能係另一個 WebMaster）。",
                  file=sys.stderr)
            print("[esp_webmaster] 用 --port 8011 換埠，或者開啟自動換埠："
                  "settings.json 的 server.auto_port=true（或加 --auto-port）。",
                  file=sys.stderr)
            return 6

    # ── 連線目標：CLI > 設定檔 ──────────────────────────────────────────
    scan = None
    if args.sim:
        autoconnect = "sim"
    elif args.serial:
        autoconnect = args.serial
    else:
        target = dev["autoconnect"]
        if isinstance(target, str) and target.strip().lower() == "auto":
            # ★ 真實優先：掃 USB 中繼資料揀板；冇板就「唔連」，唔會偷偷開模擬
            scan = link.scan_ports(prefer=dev.get("prefer_port") or None)
            autoconnect = scan["chosen"]
        else:
            autoconnect = target
        if autoconnect is None and dev["sim_dir"]:
            # 有 sim_dir 但唔係 auto：當係想用模擬器
            autoconnect = "sim"

    if args.list_ports:
        scan = scan or link.scan_ports(prefer=dev.get("prefer_port") or None)
        for line in link.format_scan(scan):
            print(line)
        return 0

    if scan:
        for line in link.format_scan(scan):
            print(f"[esp_webmaster] {line}")
        if autoconnect is None:
            print("[esp_webmaster] 唔自動連（唔會開模擬）；"
                  "插好板再喺 UI 揀 port，或者用 --sim。")

    sim_dir = args.sim_dir or dev["sim_dir"]
    app = create_app(sim_root=sim_dir, autoconnect=autoconnect)
    app.state.settings = conf
    app.state.settings_files = used

    url = f"http://{host}:{port}/"
    if open_browser:
        threading.Timer(1.2, lambda: webbrowser.open(url)).start()

    src = f"（設定：{'、'.join(used)}）" if used else "（全部用內建預設）"
    print(f"[esp_webmaster] {url} {src}")

    # ── 開機環境自檢 → 有問題就自動補（唔阻塞、唔無限重試）──────────────
    # auto_install：off=只檢查；safe=自動補 deps/esptool；all=連 IDF 都自動裝。
    # 期望管理：IDF 是 1–2 GB／10–30 分鐘，所以 safe 模式只會補「便宜又必需」的東西；
    # 想開機就裝 IDF，把 settings.json 的 env.auto_install 改成 "all"。
    if not args.no_env_check:
        try:
            rep = env.report()
            ready = rep.get("ready") or {}
            miss = [x for x in rep.get("tools", []) if x.get("status") == "missing"]
            print("[esp_webmaster] 環境：" +
                  " ".join(f"{k}={'v' if v else 'x'}" for k, v in ready.items()))
            for x in miss[:6]:
                print(f"[esp_webmaster]   缺 {x.get('label') or x['id']}"
                      f"{('（' + x['impact'] + '）') if x.get('impact') else ''}")

            auto_mode = str((conf.get("env") or {}).get("auto_install") or "safe")
            if auto_mode == "off":
                if miss:
                    print("[esp_webmaster]   自動安裝已關（env.auto_install=off）；"
                          "要裝就去 UI「環境」頁。")
            else:
                plan = env.auto_plan(auto_mode, rep)
                if plan:
                    runnable = []
                    for item in plan:
                        if item.get("blocked"):
                            print(f"[esp_webmaster] 唔可以自動補 {item['what']}：{item['reason']}")
                            if item.get("hint"):
                                print(f"[esp_webmaster]   → {item['hint']}")
                        else:
                            runnable.append(item)
                            print(f"[esp_webmaster] 自動補：{item['what']}（{item['reason']}）")
                    plan = runnable
                    if auto_mode != "all":
                        print("[esp_webmaster]   註：safe 模式唔會自動裝 ESP-IDF；"
                              "要開機自動裝 IDF 請設 env.auto_install=all")
                    targets = ",".join((conf.get("env") or {}).get("idf_targets")
                                       or env.DEFAULT_TARGETS)
                    version = (conf.get("env") or {}).get("idf_version")
                    idf_dir = (conf.get("env") or {}).get("idf_dir")

                    def _auto(plan=plan, targets=targets, version=version,
                              idf_dir=idf_dir):
                        # 逐個裝；失敗唔好連鎖
                        for item in plan:
                            what = item["what"]
                            try:
                                env.INSTALLER.start(
                                    what,
                                    target=targets if what == "idf" else "all",
                                    idf_dir=Path(idf_dir) if (what == "idf" and idf_dir) else None,
                                    confirm=True, version=version if what == "idf" else None)
                                while env.INSTALLER.running:
                                    time.sleep(0.5)
                                rc = env.INSTALLER.rc
                                # 「環境／設定問題」（資料夾唔可寫、缺 git…）唔記冷卻：
                                # 用戶改完 settings 重開就應該即刻再試。
                                if not env.INSTALLER.env_error:
                                    env.auto_mark(what, rc == 0)
                                else:
                                    print(f"[esp_webmaster] {what} 遇到環境問題（rc={rc}），"
                                          f"唔會冷卻；修正後重開即再試")
                                print(f"[esp_webmaster] 自動補 {what}："
                                      f"{'完成' if env.INSTALLER.rc == 0 else '失敗 rc=' + str(env.INSTALLER.rc)}")
                            except Exception as e2:
                                env.auto_mark(what, False)
                                print(f"[esp_webmaster] 自動補 {what} 出錯：{e2}")

                    if not plan:
                        print("[esp_webmaster]   冇可自動執行的項目（見上面說明）。")
                    else:
                        threading.Thread(target=_auto, daemon=True).start()
                elif miss:
                    only_idf = {x["id"] for x in miss} <= {"idf", "idf_toolchain"}
                    if only_idf and auto_mode != "all":
                        print("[esp_webmaster]   只缺 ESP-IDF（唔影響燒錄）；"
                              "safe 模式唔會自動裝 1–2 GB。要開機自動裝："
                              "settings.json 設 env.auto_install=\"all\"，或用 UI「環境」頁。")
                    else:
                        print("[esp_webmaster]   呢啲項目 6 小時內試過／唔可自動補；"
                              "詳情睇 UI「環境」頁。")
        except Exception as e:                       # 自檢失敗唔應該阻住啟動
            print(f"[esp_webmaster] 環境自檢失敗（略過）：{e}")

    if autoconnect == "sim":
        print("[esp_webmaster] 模擬裝置已連線（虛擬 FAT）")
    elif autoconnect:
        print(f"[esp_webmaster] 嘗試連線 {autoconnect}")
    print(f"[esp_webmaster] flash 預設：chip={fl['esptool_chip'] or '自動'} "
          f"baud={fl['esptool_baud']} before={fl['esptool_before']}")
    uvicorn.run(app, host=host, port=port, log_level=log_level)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
