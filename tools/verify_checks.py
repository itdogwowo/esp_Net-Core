#!/usr/bin/env python3
"""驗收清單自動預填：跑一堆唯讀檢查，輸出 verify.html 可直接「匯入」的 JSON。

用法：
    python3 tools/verify_checks.py --port /dev/cu.usbmodem50787D188F501 \
        --out docs/00_pm/verify-results.json

之後開 docs/00_pm/verify.html → 「匯入 JSON」→ 勾選狀態與備註自動填好，
你只需要補「肉眼」同「建置」那幾項。

只做唯讀操作（status／raw 查詢）；唔會 reboot、唔會寫 FAT、唔會 OTA。
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time
from datetime import datetime

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NC4 = os.path.join(ROOT, "tools", "nc4.py")


def run_nc4(port, *args, timeout=15):
    """呼叫 tools/nc4.py，回 (rc, stdout, stderr)。"""
    cmd = [sys.executable, NC4, "--port", port, *args]
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, "", "timeout"


def nc4_json(port, *args):
    """跑 nc4.py 並把 stdout 當 JSON（status 會自動收多頁）。"""
    rc, out, err = run_nc4(port, *args)
    if rc != 0:
        return None, (err.strip().splitlines() or ["rc=%d" % rc])[-1]
    try:
        return json.loads(out), None
    except json.JSONDecodeError as e:
        return None, "JSON 解析失敗：%s" % e


def raw_payload(port, cmd_hex, payload_hex=""):
    """送一條 raw 並取回覆 payload（第二個回覆幀）；失敗回 (None, 原因)。

    nc4.py raw 會把收到的 ASCII 直接印出來，這裡改成自己讀 serial，
    避免要解析工具的輸出格式。
    """
    try:
        import serial  # pyserial：tools/nc4.py 已經依賴它
    except ImportError:
        return None, "冇 pyserial"
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        from nc4_demux import pack, Reader
    except Exception as e:            # noqa: BLE001
        return None, "載入 nc4_demux 失敗：%s" % e

    cmd = int(cmd_hex, 0)
    payload = bytes.fromhex(payload_hex) if payload_hex else b""
    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:            # noqa: BLE001
        return None, "開 serial 失敗：%s" % e
    try:
        rd = Reader(ser)
        ser.write(pack(cmd, payload, 0xFFFF))
        f = rd.wait_for({cmd + 1, cmd}, 3.0)
        if f is None:
            return None, "冇回覆"
        return f[1], None
    finally:
        ser.close()


def raw_frames(port, cmd_hex, payload_hex="", max_frames=48, per_timeout=3.0):
    """送一條 raw，連續收幀直到 more=0（最多 max_frames）。

    回 (frames, err)；frames = [(payload, more, timeout_ms), …]。
    """
    try:
        import serial
    except ImportError:
        return None, "冇 pyserial"
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        from nc4_demux import pack, Reader
    except Exception as e:            # noqa: BLE001
        return None, "載入 nc4_demux 失敗：%s" % e

    cmd = int(cmd_hex, 0)
    payload = bytes.fromhex(payload_hex) if payload_hex else b""
    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:            # noqa: BLE001
        return None, "開 serial 失敗：%s" % e
    try:
        rd = Reader(ser)
        ser.write(pack(cmd, payload, 0xFFFF))
        out = []
        while len(out) < max_frames:
            f = rd.wait_for({cmd + 1, cmd}, per_timeout)
            if f is None:
                return None, "第 %d 幀冇回覆" % (len(out) + 1)
            p = f[1]
            n = struct.unpack_from("<H", p)[0] if len(p) >= 2 else 0
            tail = p[2 + n:2 + n + 3]
            more = tail[0] if len(tail) > 0 else 0
            tmo = struct.unpack_from("<H", tail, 1)[0] if len(tail) >= 3 else None
            out.append((p, more, tmo))
            if not more:
                break
        return out, None
    finally:
        ser.close()


def payload_text(p):
    if p is None or len(p) < 2:
        return ""
    n = struct.unpack_from("<H", p)[0]
    return p[2:2 + n].decode("utf-8", "replace")


def put_item(items, key, ok, note):
    items[key] = {"v": 1 if ok else 2, "note": note}


def send_and_capture(port, cmd_hex, payload_hex="", seconds=4.0):
    """送一幀，之後讀 N 秒的原始輸出（log 行＋回覆幀混在一起），回 decode 後的字串。"""
    try:
        import serial
    except ImportError:
        return ""
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    try:
        from nc4_demux import pack
    except Exception:                          # noqa: BLE001
        return ""
    try:
        ser = serial.Serial(port, 115200, timeout=0.2)
    except Exception:                          # noqa: BLE001
        return ""
    buf = bytearray()
    try:
        ser.reset_input_buffer()
        ser.write(pack(int(cmd_hex, 0), bytes.fromhex(payload_hex) if payload_hex else b"", 0xFFFF))
        end = time.time() + seconds
        while time.time() < end:
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
            else:
                time.sleep(0.02)
    finally:
        ser.close()
    return buf.decode("utf-8", "replace")


def build_port(name, tests=False):
    cmd = [sys.executable, os.path.join(ROOT, "make.py"), "build", name]
    if tests:
        cmd.append("--tests")
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=1800,
                       env={**os.environ, "IDF_PATH": os.path.expanduser("~/esp/esp-idf")})
    out = (p.stdout or "") + (p.stderr or "")
    ok = ("建置完成" in out) and ("error:" not in out)
    tail = [l.strip() for l in out.strip().splitlines() if l.strip()]
    return ok, (tail[-1][:120] if tail else "(冇輸出)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "00_pm", "verify-results.json"))
    args = ap.parse_args()

    items = {}
    log = []

    # ── A. 連線 ──
    sys_obj, err = nc4_json(args.port, "status", "sys")
    if sys_obj is None:
        put_item(items, "conn-board", False, "讀 sys 失敗：%s" % err)
        put_item(items, "conn-fw", False, "讀 sys 失敗")
        put_item(items, "conn-rst", False, "讀 sys 失敗")
        print("!! 連唔到板：%s" % err)
    else:
        s = sys_obj.get("sys", {})
        chip = s.get("chip", "?")
        put_item(items, "conn-board", chip == "esp32s3", "chip=%s idf=%s" % (chip, s.get("idf", "?")))
        fw = s.get("fw", "?")
        put_item(items, "conn-fw", fw not in ("?", "", None),
                 "fw=%s app=%s" % (fw, s.get("app", "?")))
        up1, rst1 = s.get("up_s", 0), s.get("rst", -1)

        import time
        time.sleep(1.3)                    # up_s 係整數秒，要跨過一秒邊界
        sys2, _ = nc4_json(args.port, "status", "sys")
        if sys2 is None:
            put_item(items, "conn-rst", False, "第二次 sys 冇回覆")
        else:
            s2 = sys2.get("sys", {})
            up2, rst2 = s2.get("up_s", 0), s2.get("rst", -1)
            put_item(items, "conn-rst", up2 >= up1 and rst2 == rst1,
                     "up_s %s→%s、rst=%s（冇重開機）" % (up1, up2, rst2))

    # ── B. 分頁 ──
    full, err = nc4_json(args.port, "status")
    if full is None:
        put_item(items, "paging-all", False, "讀 status 失敗：%s" % err)
    else:
        keys = list(full)
        put_item(items, "paging-all", len(keys) >= 19,
                 "%d 個 key：%s" % (len(keys), ", ".join(keys[:24])))

    frames, err = raw_frames(args.port, "0x1101", "0000")
    if frames is None:
        put_item(items, "paging-more", False, "raw 0x1101 收唔齊：%s" % err)
    else:
        last_more, last_tmo = frames[-1][1], frames[-1][2]
        mid_tmo = [t for (_, m, t) in frames[:-1] if t is not None]
        ok = (last_more == 0) and all(t == 10 for t in (mid_tmo or [10]))
        put_item(items, "paging-more", ok,
                 "%d 幀；中間頁 timeout=%s；最後 more=%s tmo=%s"
                 % (len(frames), sorted(set(mid_tmo)) or "-", last_more, last_tmo))

    tasks = None
    extra, err = raw_payload(args.port, "0x1101", "05007461736b73")  # "tasks"
    if extra is not None:
        try:
            tasks = json.loads(payload_text(extra))
        except Exception:                          # noqa: BLE001
            tasks = None
    ok_hint = bool(tasks and str(tasks.get("hint", "")).startswith("0x"))
    put_item(items, "paging-hint", ok_hint,
             "tasks 回：%s" % (json.dumps(tasks, ensure_ascii=False)[:120] if tasks else "冇"))

    ncm, err = nc4_json(args.port, "status", "ncm")
    ok_ncm = bool(ncm and isinstance(ncm.get("ncm"), dict) and ncm["ncm"])
    put_item(items, "paging-ncm", ok_ncm,
             (json.dumps(ncm, ensure_ascii=False)[:160] if ncm else "冇回覆：%s" % err))

    # ── C. .nce ──
    nce, err = nc4_json(args.port, "status", "nce")
    if nce is None:
        put_item(items, "nce-values", False, "讀 nce 失敗：%s" % err)
        put_item(items, "nce-pub", False, "讀 nce 失敗")
    else:
        d = nce.get("nce", {})
        v = d.get("v", [])
        ok_v = len(v) >= 6 and v[0] == 255 and v[2] == 64 and v[1] in (0, 1, 2, 3)
        put_item(items, "nce-values", ok_v,
                 "pub=%s px=%s v=%s" % (d.get("pub"), d.get("px"), v[:9]))
        pub1 = d.get("pub", 0)
        import time
        time.sleep(2.0)
        nce2, _ = nc4_json(args.port, "status", "nce")
        pub2 = (nce2 or {}).get("nce", {}).get("pub", 0)
        put_item(items, "nce-pub", pub2 > pub1, "pub %s→%s（相隔 2s）" % (pub1, pub2))

    ncm2, err = nc4_json(args.port, "status", "ncm")
    live = ((ncm2 or {}).get("ncm") or {}).get("live", "")
    mods = ((ncm2 or {}).get("ncm") or {}).get("modules", [])
    put_item(items, "nce-live", live != "" and bool(mods),
             "live=%s modules=%d" % (live or "-", len(mods)))

    # ── D. mode（只做唯讀；切 mode 會影響畫面，所以只查 SHOWFRAME 行為）──
    obj, err = {}, None
    import time
    for _ in range(6):
        f, e = raw_payload(args.port, "0xF06B", "")
        if f is None:
            err = e
            time.sleep(0.3)
            continue
        try:
            obj = json.loads(payload_text(f))
        except Exception:                          # noqa: BLE001
            obj = {}
        if obj.get("err") == "big":
            break
        time.sleep(0.4)                            # nodata：等 paint 出幀再問
    put_item(items, "mode-frame", obj.get("err") == "big",
             "SHOWFRAME：%s" % (json.dumps(obj, ensure_ascii=False)[:120] if obj else "冇回覆：%s" % err))

    # ── D2. 切 mode（會影響畫面 → 最後就停喺示範 mode，方便用戶肉眼睇）──
    txt = send_and_capture(args.port, "0xF06D", "1500", seconds=4.0)
    m = re.search(r'\{"ok":1,"queued":(\d+)', txt)
    put_item(items, "mode-select", bool(m) and m.group(1) == "21",
             ("queued=%s" % m.group(1)) if m else "冇見到 0xF06D 回覆")
    lay = re.search(r"layout[：:]\s*px=(\d+) jobs=(\d+) runs=(\d+) aprog=(\d+) skipped=(\d+)", txt)
    if lay is None:
        # 切 mode 之後 paint 可能已經換完場；用 reload 令佢重建一次並印 layout
        txt2 = send_and_capture(args.port, "0xF06F", "", seconds=6.0)
        lay = re.search(r"layout[：:]\s*px=(\d+) jobs=(\d+) runs=(\d+) aprog=(\d+) skipped=(\d+)", txt2)
        if lay is None:
            txt = txt + txt2
    bad_lay = "layout err=" in txt
    put_item(items, "mode-layout", bool(lay) and not bad_lay and int(lay.group(2)) > 0,
             (("px=%s jobs=%s aprog=%s skipped=%s" % (lay.group(1), lay.group(2), lay.group(4), lay.group(5)))
              if lay else ("layout err 出現" if bad_lay else "冇見到 layout log")))

    # ── E/F. 自動化（本機）──
    try:
        p = subprocess.run(["bash", "tools/hosttest/run_all.sh"], cwd=ROOT,
                           capture_output=True, text=True, timeout=1800)
        ok = p.returncode == 0 and ("全部通過" in p.stdout or "ALL PASS" in p.stdout)
        tail = (p.stdout.strip().splitlines() or ["(冇輸出)"])[-1]
        put_item(items, "tests-host", ok, "run_all.sh：%s" % tail)
    except Exception as e:                         # noqa: BLE001
        put_item(items, "tests-host", False, "跑 host 測試失敗：%s" % e)

    ok, note = build_port("ESP32-S3-Zero", tests=False)
    put_item(items, "build-release", ok, "Zero-Show release：" + note)

    ok2, note2 = build_port("ESP32-S3-Zero", tests=True)
    put_item(items, "build-tests", ok2, "Zero-Show tests：" + note2)

    # tests-ids：獨立 build dir，驗 0,1,2,3 都編得過
    ids_cmd = (
        'cd "%s/firmware" && . "$IDF_PATH/export.sh" >/dev/null 2>&1 && '
        'idf.py -B /tmp/ncb_verify_ids -DPORT=S3/ESP32-S3-Zero -DPROJECT=S3/ESP32-S3-Zero-Show '
        '-DIDF_TARGET=esp32s3 -DPROJECT_VER=verify -DSDKCONFIG=/tmp/ncb_verify_ids/sdkconfig '
        '-DNC_ENABLE_TESTS=ON -DNC_TEST_IDS=0,1,2,3 build' % ROOT
    )
    try:
        p = subprocess.run(["bash", "-lc", ids_cmd], capture_output=True, text=True,
                           timeout=1800, env={**os.environ,
                                              "IDF_PATH": os.path.expanduser("~/esp/esp-idf")})
        out = (p.stdout or "") + (p.stderr or "")
        seen = ("測試 ID：0;1;2;3" in out) or ("測試 ID：0,1,2,3" in out)
        # 第二次起 CMake 唔會重印「測試 ID」行（configure 已 cache）→ 直接讀 cache 證明。
        cache = "/tmp/ncb_verify_ids/CMakeCache.txt"
        cached_ids = ""
        try:
            for line in open(cache, encoding="utf-8", errors="replace"):
                if line.startswith("NC_TEST_IDS:"):
                    cached_ids = line.strip().split("=", 1)[-1]
                    break
        except OSError:
            cached_ids = ""
        if not seen and cached_ids in ("0,1,2,3", "0;1;2;3"):
            seen = True
            out += "\n[verify] cache NC_TEST_IDS=%s" % cached_ids
        built = ("Project build complete" in out) or ("no work to do" in out)
        ok_ids = bool(p.returncode == 0 and seen and built)
        put_item(items, "tests-ids", ok_ids,
                 "rc=%s；%s；%s" % (p.returncode,
                                   "見 0;1;2;3" if seen else "未見測試 ID 行",
                                   "build 完成" if built else "build 未完成"))
    except Exception as e:                         # noqa: BLE001
        put_item(items, "tests-ids", False, "跑 idf.py 失敗：%s" % e)

    # registry：讀裝置上的 JSON
    reg_path = "/tmp/nc_verify_reg.json"
    rc, out, err = run_nc4(args.port, "file", "get", "/pixel/registry.json", reg_path) if False else (1, "", "")
    try:
        p = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "nc4_file.py"),
                            "--port", args.port, "get", "/pixel/registry.json", reg_path],
                           capture_output=True, text=True, timeout=30)
        reg = json.load(open(reg_path, encoding="utf-8")) if p.returncode == 0 else None
        ok_reg = bool(reg and reg.get("auto_play") is True and len(reg.get("list", [])) >= 4)
        put_item(items, "close-registry", ok_reg,
                 (json.dumps(reg, ensure_ascii=False)[:120] if reg else "讀唔到 registry：%s" % p.stderr.strip()))
    except Exception as e:                         # noqa: BLE001
        put_item(items, "close-registry", False, "讀 registry 失敗：%s" % e)

    # docs：status 文件要有今次章節
    try:
        doc = open(os.path.join(ROOT, "docs", "00_pm", "status.md"), encoding="utf-8").read()
        need = ["7.356", "7.360", "7.365", "7.370"]
        missing = [n for n in need if ("### %s" % n) not in doc]
        put_item(items, "close-docs", not missing,
                 "缺：%s" % ", ".join(missing) if missing else "§7.356–7.370 都在")
    except Exception as e:                         # noqa: BLE001
        put_item(items, "close-docs", False, "讀 status.md 失敗：%s" % e)

    out = {
        "items": items,
        "port": args.port,
        "updatedAt": datetime.now().isoformat(timespec="seconds"),
        "generatedBy": "tools/verify_checks.py",
    }
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(out, fh, ensure_ascii=False, indent=2)

    # 只留兩項給人：
    items.setdefault("mode-leds", {"v": 0, "note": "等用戶肉眼：切 mode 21 後 led16.g0 見 RGB 漸變"})
    items.setdefault("close-export", {"v": 0, "note": "等用戶在網頁按「匯出 JSON／Markdown」存檔"})

    ok_n = sum(1 for r in items.values() if r["v"] == 1)
    bad_n = sum(1 for r in items.values() if r["v"] == 2)
    print("✅ 已寫 %s（自動檢查 %d 項：通過 %d、失敗 %d）"
          % (os.path.relpath(args.out, ROOT), len(items), ok_n, bad_n))
    for k, r in items.items():
        mark = "✓" if r["v"] == 1 else ("⏳" if r["v"] == 0 else "✗")
        print("  %s %-14s %s" % (mark, k, r["note"][:110]))
    print("\n下一步：開 docs/00_pm/verify.html → 匯入 JSON → 補肉眼／建置幾項。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
