#!/usr/bin/env python3
"""ESP WebMaster 啟動入口。

    python run.py --sim                 # 模擬裝置（唔使板）
    python run.py --serial COM13        # 直接連 CDC（P4 OTG / S3 USB）
    python run.py --port 8010 --open    # 自訂 HTTP port + 自動開瀏覽器
"""
from __future__ import annotations

import argparse
import threading
import webbrowser

import uvicorn

from server import create_app


def main() -> int:
    ap = argparse.ArgumentParser(description="ESP WebMaster")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8010, help="HTTP port（預設 8010）")
    ap.add_argument("--serial", default=None, metavar="COMx", help="啟動即連線的序列埠")
    ap.add_argument("--sim", action="store_true", help="用模擬裝置（虛擬 FAT）")
    ap.add_argument("--sim-dir", default=None, help="模擬裝置的根資料夾")
    ap.add_argument("--open", action="store_true", help="啟動後開瀏覽器")
    args = ap.parse_args()

    autoconnect = "sim" if args.sim else args.serial
    app = create_app(sim_root=args.sim_dir, autoconnect=autoconnect)

    url = f"http://{args.host}:{args.port}/"
    if args.open:
        threading.Timer(1.2, lambda: webbrowser.open(url)).start()

    print(f"[esp_webmaster] {url}")
    if args.sim:
        print("[esp_webmaster] 模擬裝置已連線（虛擬 FAT）")
    elif args.serial:
        print(f"[esp_webmaster] 嘗試連線 {args.serial}")
    uvicorn.run(app, host=args.host, port=args.port, log_level="info")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
