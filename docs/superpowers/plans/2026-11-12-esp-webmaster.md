# ESP WebMaster（獨立 UI ＋ NC4 檔案指令）Implementation Plan

> **最後修訂（2026-11-12 晚）：** 用户要求**完全跟 schema，唔加任何新指令 ID**。
> 所以下面 Task 7 提到的 `0x2012/0x2013 FILE_LIST` **已移除**；改用 schema 本身
> 的 `0x200B FILE_SCAN` + `/manifest.json`（跟 mp 版），UI 一次讀 manifest 砌樹。
> 其餘 `0x2001–0x200A` 不變。

> **For agentic workers:** 這份計劃由同一個 session 直接執行；每一步都係可獨立驗證嘅小型改動。
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 喺 `esp_Net-Core` 內建一套**完全獨立**（唔依賴 mp_Net-Core）嘅 ESP 專用 Web UI，外觀照抄 WebMaster；同時補齊 C firmware 嘅 NC4 檔案指令，令 ESP 經 USB CDC 有完整 MPY 式體驗：拖拉上傳、下載、編輯、confirm/undo、log 終端、console、OTA。

**Architecture:**
- 新工具 `tools/esp_webmaster/`：FastAPI ＋ 原生 JS SPA（零 build step）。後端用 pyserial 開 CDC，自己做 NC4 分幀、請求／回應配對、log 行（RS 0x1E）分流；前端只講 JSON/HTTP/WS。
- Firmware：`nc_file_action.c` 由「只有 FILE_QUERY」擴充到完整 `0x2001–0x200A`，另加 `0x2012/0x2013 FILE_LIST`（目錄列舉，UI 檔案樹用）。
- 無硬件都可玩：`--sim` 用虛擬 FAT（temp dir）＋同一個 NC4 介面，測試與示範唔需要板。

**Tech Stack:** Python 3.10+、FastAPI、uvicorn、pyserial、原生 HTML/CSS/JS、ESP-IDF（C 側）、pytest。

**環境限制（重要）:** 本機**冇 ESP-IDF**（`IDF_PATH` 空、`idf.py` 不在 PATH）。所以 firmware 只可以做到「source 完成＋人手 review」，真正 compile 要喺有 IDF 嘅機器跑 `python make.py build s3`。UI 同 protocol 可以喺本機完整測試。

---

## 修訂（2026-11-12 下午）：CDC FAT 為主，MSC 只係可選

用户澄清：這個工具是為了**部分 MCU 冇 USB driver、只能經 CDC 操作**。
所以檔案一定要走 NC4 FAT 指令，唔可以倚賴 MSC：

- Task 7 **恢復並收窄**：實作 `FILE_LIST(0x2012/0x2013)`、`FILE_READ(0x2007)`、
  `FILE_BEGIN/CHUNK/END/ACK(0x2001–0x2004)`、`FILE_CONFIRM/DELETE/UNDO(0x2008–0x200A)`。
  唔做 `FILE_MOVE`、`FILE_PROMOTE`、斷點續傳、SD 寫入。
- UI「檔案」頁係主路徑（CDC）；「隨身碟」頁只係有 USB 的板子的可選捷徑。
- 寫入語義：`.part` → sha256 → 舊檔 `.bak` → 新檔上位；pending = `.bak` 存在。
- `0x1103 file_mode` 仍然用嚟接管 FAT（USB 模式寫入會 `err_busy`）。

---

## File Structure

```
esp_Net-Core/
├─ tools/esp_webmaster/
│  ├─ nc4.py                 # NC4 wire codec（由 tools/nc4_demux.py 精簡／固定）
│  ├─ link.py                # SerialLink / SimLink：reader thread、request、log 分流
│  ├─ files.py               # FILE_* client（query/list/upload/download/delete/confirm/undo）
│  ├─ ota.py                 # OTA client（0x22xx）
│  ├─ sim.py                 # SimDevice：虛擬 FAT＋NC4 handler（無硬件）
│  ├─ server.py              # FastAPI：REST + WS + 靜態 SPA
│  ├─ run.py                 # CLI 入口（--port / --sim / --open）
│  ├─ launch.py              # 一鍵：venv + 裝 deps + 開瀏覽器
│  ├─ requirements.txt
│  ├─ README.md
│  ├─ static/{index.html,app.js,style.css}
│  └─ tests/{test_nc4.py,test_files.py,test_server.py}
├─ firmware/components/action/file_action/nc_file_action.c   # 修改
└─ docs/03_notes/file-transfer.md                             # 新增
```

**邊界：**
- `nc4.py` 只管 bytes↔frame，唔知檔案。
- `link.py` 只管一條連線（serial 或 sim），請求序列化，log/frame 分流。
- `files.py` / `ota.py` 只講協議流程，唔知 HTTP。
- `server.py` 只做編排＋安全檢查，唔直接砌 frame。
- 前端只經 REST/WS，唔碰 serial。

---

## Task 1: NC4 codec 與傳輸層

**Files:**
- Create: `tools/esp_webmaster/nc4.py`
- Create: `tools/esp_webmaster/link.py`
- Test: `tools/esp_webmaster/tests/test_nc4.py`

**協議（同 firmware 一致）：**
```
"NC" | VER=4 | ADDR(2 LE) | CMD(2 LE) | LEN(2 LE) | DATA | CRC32(4 LE)
CRC = crc32(VER..DATA)，唔包 SOF / CRC
log 行 = 0x1E + UTF-8 + '\n'，同幀交錯
```

- [ ] **Step 1: 寫 failing test（frame roundtrip / 假 SOF / CRC 錯 / 切開讀 / log 行）**

```python
# tools/esp_webmaster/tests/test_nc4.py
import struct
from nc4 import Demux, pack, str_u16, decode_str_u16

def test_pack_roundtrip():
    raw = pack(0x1101, str_u16("sys"))
    d = Demux()
    ev = d.feed(raw)
    assert ev == [("frame", (0x1101, str_u16("sys")))]

def test_fake_sof_then_real_frame():
    d = Demux()
    ev = d.feed(b"NC" + b"\x00" * 3 + pack(0x100F))
    assert [e[0] for e in ev] == ["frame"]
    assert ev[0][1][0] == 0x100F

def test_bad_crc_keeps_scanning():
    good = pack(0x100F)
    bad = bytearray(good); bad[-1] ^= 0xFF
    d = Demux()
    ev = d.feed(bytes(bad) + good)
    assert len([e for e in ev if e[0] == "frame"]) == 1

def test_split_frame():
    raw = pack(0x1101, b"hello")
    d = Demux()
    assert d.feed(raw[:5]) == []
    ev = d.feed(raw[5:])
    assert ev[0][1][0] == 0x1101

def test_log_line_mixed_with_frame():
    d = Demux()
    ev = d.feed(b"\x1eI (123) boot: hi\n" + pack(0x1102, str_u16('{"a":1}')))
    kinds = [e[0] for e in ev]
    assert kinds == ["line", "frame"]
    assert ev[0][1] == "I (123) boot: hi"
```

- [ ] **Step 2: 跑 test，確認 fail（module 未存在）**

Run: `cd tools/esp_webmaster && python -m pytest tests/test_nc4.py -q`
Expected: FAIL `ModuleNotFoundError: nc4`

- [ ] **Step 3: 實作 `nc4.py`**

由 `tools/nc4_demux.py` 複製並收窄（保留嚴格規則：失敗只前進 1 byte；`\x1e` 行）；對外 export：`SOF/VERSION/ADDR_BROADCAST/MAX_PAYLOAD/pack/str_u16/decode_str_u16/Demux`。

- [ ] **Step 4: 實作 `link.py`**

```python
class LinkError(RuntimeError): ...

class BaseLink:
    def send(self, cmd: int, payload: bytes = b"") -> None: ...
    def request(self, cmd, payload=b"", expect=None, timeout=3.0) -> tuple[int, bytes]: ...
    def close(self) -> None: ...
    on_line: Callable[[str], None] | None
    on_frame: Callable[[int, bytes], None] | None

class SerialLink(BaseLink):
    def __init__(self, port: str, baud: int = 115200): ...
    # reader thread: in_waiting 迴圈 → Demux.feed → line 派 on_line；frame 入 self._frames
    # request(): with self._req_lock: 清空舊幀 → send → 等 expect（預設 {(cmd|0x0001), cmd}？）
    #   ↑ 明確傳 expect；唔好靠猜。等待期間收到嘅其他幀交 on_frame。

class SimLink(BaseLink):
    def __init__(self, device): ...   # device.handle(cmd, payload) -> (cmd, payload)
```

`request` 規則：
1. 同一個 instance 一次只准一個 request（`threading.Lock`）。
2. `expect` 係 set；收到唔 match 嘅幀照樣派 `on_frame`，唔可以丟（log 終端要看到）。
3. timeout 拋 `LinkError("timeout waiting 0x....")`。
4. DTR：開埠後 `time.sleep(0.15)`；CDC 要 DTR 先收（TinyUSB）。

- [ ] **Step 5: 跑 test 全綠**

Run: `cd tools/esp_webmaster && python -m pytest tests/test_nc4.py -q`
Expected: `5 passed`

---

## Task 2: 檔案 client（MPY 同款流程）

**Files:**
- Create: `tools/esp_webmaster/files.py`
- Test: `tools/esp_webmaster/tests/test_files.py`（用 Task 3 嘅 sim；先寫 API）

**協議流程（照 mp_Net-Core `file_actions.py`，C 側對齊）：**
```
upload:   FILE_QUERY(0x2005) → FILE_BEGIN(0x2001) → [FILE_CHUNK(0x2002) → FILE_ACK(0x2004)]* → FILE_END(0x2003) → 0x2006 | 0x2010
download: FILE_QUERY(0x2005) → [FILE_READ(0x2007) → 0x2002(file_id=0, offset, data)]*
list:     FILE_LIST(0x2012) → 0x2013
delete:   FILE_DELETE(0x2009) → 0x2006
confirm:  FILE_CONFIRM(0x2008) → 0x2006
undo:     FILE_UNDO(0x200A) → 0x2006
```

- [ ] **Step 1: 定義 API 與錯誤型別**

```python
class FileError(RuntimeError):
    def __init__(self, kind: str, message: str = "", detail: dict | None = None): ...

def query(link, path: str, timeout=5.0) -> dict
    # {"exists":bool,"sha256":bytes,"size":int,"path":str,"free":int,"pending":bool}

def list_dir(link, path: str = "/", timeout=5.0) -> dict
    # {"path":str,"entries":[{"n":str,"d":bool,"s":int}],"more":bool}

def upload(link, path: str, data: bytes, chunk_size: int = 4096,
           progress: Callable[[int, int], None] | None = None) -> dict
    # 1) query；2) BEGIN；3) 每 chunk 等 ACK（offset 回聲要等於送出的 offset）；
    # 4) END 等 0x2006；任何 0x2010 → FileError(kind, detail)

def download(link, path: str, chunk_size: int = 4096,
             progress: Callable[[int, int], None] | None = None) -> bytes
def delete(link, path) -> dict
def confirm(link, path) -> dict
def undo(link, path) -> dict
```

- [ ] **Step 2: 錯誤碼對映（0x2010 七個 bool → 人類訊息）**

```python
ERROR_FIELDS = ["err_no_space","err_write_fail","err_offset_mismatch",
                "err_id_mismatch","err_sha_mismatch","err_not_active","err_busy"]
```

`err_busy` → `FileError("busy", "FAT 唔係裝置手上：先喺「設備」頁按「接管 FAT」或熄 MSC")`。

- [ ] **Step 3: 寫 sim 版 E2E test（Task 3 完成後補跑）**

```python
def test_upload_download_roundtrip(sim_link):
    files.upload(sim_link, "/config/hello.json", b'{"x":1}')
    assert files.download(sim_link, "/config/hello.json") == b'{"x":1}'
    q = files.query(sim_link, "/config/hello.json")
    assert q["exists"] and q["size"] == 7
```

- [ ] **Step 4: 跑 test**

Run: `cd tools/esp_webmaster && python -m pytest tests/test_files.py -q`
Expected: PASS

---

## Task 3: 模擬裝置（無硬件都可示範）

**Files:**
- Create: `tools/esp_webmaster/sim.py`

**設計：**
- `SimDevice(root: Path)`：真係用 host filesystem 做「裝置 FAT」，路徑 `/data/...` 對應 `root/...`。
- `handle(cmd, payload) -> (rsp_cmd, rsp_payload)`：實作 0x1101/0x1103/0x2001–0x200A/0x2012/0x22xx/0x100F。
- 每次操作 `emit_line("I (sim) ...")`，令 UI 終端有嘢睇。
- `system.json` / `network.json` / `hardware.json` / `pixel/effects/*.json` 種子檔。
- OTA：收完寫 `root/firmware.bin`，回 STATE_RSP verified。
- 錯誤行為要同 firmware 一致（offset 唔啱 → 0x2010 err_offset_mismatch；sha 錯 → err_sha_mismatch）。

- [ ] **Step 1: 實作 SimDevice＋SimLink**
- [ ] **Step 2: `tests/test_files.py` 全綠**
- [ ] **Step 3: 手動 smoke**

Run: `cd tools/esp_webmaster && python -c "from sim import *; ..."`（或 pytest）

---

## Task 4: OTA client

**Files:**
- Create: `tools/esp_webmaster/ota.py`

流程照 `tools/ota.py`：CAPS → BEGIN(size,chunk,sha,ver) → WRITE(offset+data)/ACK → END → STATE_RSP → APPLY。
API：`ota_update(link, data: bytes, chunk_size=4096, apply=True, progress=None) -> dict`。
錯誤：0x221C 11 個 bool → 訊息。

- [ ] **Step 1: 實作**
- [ ] **Step 2: sim test（OTA 後 firmware.bin 存在、size 相同）**

---

## Task 5: FastAPI 伺服器

**Files:**
- Create: `tools/esp_webmaster/server.py`
- Create: `tools/esp_webmaster/run.py`
- Create: `tools/esp_webmaster/requirements.txt`
- Test: `tools/esp_webmaster/tests/test_server.py`

**REST：**
| Method | Path | 說明 |
|---|---|---|
| GET | `/api/ports` | pyserial `comports()` |
| POST | `/api/connect` | `{"port":"COM13"}` 或 `{"sim":true}` |
| POST | `/api/disconnect` | |
| GET | `/api/info` | `{connected, port, sim, proto}` |
| GET | `/api/status?keys=` | 0x1101 → JSON |
| GET | `/api/files?path=/` | 0x2012 |
| GET | `/api/file?path=...` | 0x2007 全檔（bytes） |
| PUT | `/api/file?path=...` | raw body 上傳（0x2001–0x2003） |
| POST | `/api/file/delete` | |
| POST | `/api/file/confirm` | |
| POST | `/api/file/undo` | |
| POST | `/api/firmware` | raw .bin OTA；`?apply=1` |
| POST | `/api/reboot` | 0x100F |
| POST | `/api/cmd` | `{cmd:"0x1101", payload_hex:"...", expect:"0x1102", timeout:5}` |
| WS | `/ws/log` | 推送 `{"type":"line","text":...}`、`{"type":"frame","cmd":...}`、`{"type":"progress",...}` |

**規則：**
- 所有 device 操作經一把 `threading.Lock`（NC4 一條線，請求序列化）。
- 未連線 → 409 `{"ok":false,"err":"not connected"}`。
- 上傳／OTA 期間經 WS 推 progress（每 chunk 一次，UI 用）。
- 靜態：`/` → `static/index.html`；`/static/*`。

- [ ] **Step 1: 實作 server（sim 先行）**
- [ ] **Step 2: `tests/test_server.py`（TestClient + sim）：connect、list、upload、download、delete、status、ota**
- [ ] **Step 3: 跑 test**

Run: `cd tools/esp_webmaster && python -m pytest -q`
Expected: 全綠

---

## Task 6: 前端 SPA（照抄 WebMaster 外觀）

**Files:**
- Create: `tools/esp_webmaster/static/index.html`
- Create: `tools/esp_webmaster/static/style.css`（由 WebMaster `static/style.css` 複製，加檔案樹／編輯器／dropzone 樣式）
- Create: `tools/esp_webmaster/static/app.js`

**版面：**
```
topbar:  🛰 ESP WebMaster | [連線狀態] [port] [🌓]
sidebar: 序列埠清單（刷新）＋ 連線／斷線；模擬裝置按鈕；設備摘要
tabs:    ⚙ 設備 | 📁 檔案 | 🔥 固件 | ⌨ Console | 📋 終端
```

**⚙ 設備**
- sys：chip / version / uptime / heap / reset reason
- storage：mode / free / total / pending 數
- log：cap/run/lvl/produced/sent/dropped
- 按鈕：重新讀取、重啟、接管 FAT（0x1103 file_mode=1）、交還 FAT（0）、重建 FAT（fs_format，二次確認）、除錯 log 模式

**📁 檔案（重点：似隨身碟＋MPY）**
- 麪包屑 `/ config /`；`..` 上一層；folder row 點兩下入去
- 表格：名稱、大小、狀態（pending 黃牌）、操作（編輯／下載／確認／復原／刪除）
- **拖拉整批檔案入 dropzone → 上傳到當前目錄**（進度條）
- 文字檔（.json/.txt/.py/.md/.csv/.log）→ 內嵌編輯器（textarea，mono），「儲存」= 重新上傳；二進位 → 只可下載
- 「確認」= 0x2008（清 .bak）；「復原」= 0x200A
- 上傳同名檔會自動留 `.bak`；UI 顯示 pending 直到確認

**🔥 固件**
- 拖 `.bin` 或揀檔；顯示 size / sha256；「開始 OTA」；進度條；apply 勾選；完成後提示重啟

**⌨ Console**
- cmd 下拉（0x1101/0x1103/0x100F/0x2005/0x2012/0xF060…）＋ payload hex ＋ expect ＋ timeout
- 結果：hex dump ＋（似 JSON 就 pretty print）

**📋 終端**
- WS 即時 log，level 顏色（I/W/E），暫停／清除／下載 .log
- 連線事件、上傳／OTA 進度都入同一條時間線

- [ ] **Step 1: HTML 骨架＋tabs**
- [ ] **Step 2: CSS（copy + 追加）**
- [ ] **Step 3: JS：connect/status/files/editor/upload/drop/ota/console/ws**
- [ ] **Step 4: 用 sim 開瀏覽器實測（截圖）**

Run: `cd tools/esp_webmaster && python run.py --sim --port 8011 --open`
Expected: 瀏覽器見到 UI，可上傳／下載／編輯／OTA

---

## Task 7: Firmware 檔案指令（C）

**Files:**
- Modify: `firmware/components/action/file_action/nc_file_action.c`
- Modify: `firmware/components/action/file_action/CMakeLists.txt`（如需）
- Docs: `docs/01_protocol/commands.md`、`docs/03_notes/file-transfer.md`、`tools/README.md`

**新增指令：**
| cmd | 名稱 | payload |
|---|---|---|
| 0x2001 | FILE_BEGIN | file_id u16, total u32, chunk u16, sha256 32B, path str |
| 0x2002 | FILE_CHUNK | file_id u16, offset u32, data rest |
| 0x2003 | FILE_END | file_id u16 |
| 0x2004 | FILE_ACK | file_id u16, offset u32（回覆 chunk） |
| 0x2007 | FILE_READ | path str, offset u32, length u16 → 回 0x2002(file_id=0, offset, data) |
| 0x2008 | FILE_CONFIRM | path str → 0x2006 |
| 0x2009 | FILE_DELETE | path str → 0x2006 |
| 0x200A | FILE_UNDO | path str → 0x2006 |
| 0x2012 | FILE_LIST | path str → 0x2013 |
| 0x2013 | FILE_LIST_RSP | str_u16len(JSON) |

**實作要點：**
- 一把 `SemaphoreHandle_t s_lock`（`nc_file_action_init` 建立）包住所有檔案操作。
- 只准寫 `nc_store_app_ready()` 嘅 `/data`；否則 0x2010 `err_busy=1`。
- Session：`active/id/path/temp/size/chunk/written/sha_expect/psa_hash_operation_t`。
- BEGIN：檢查 chunk ≤ 4096、free ≥ total；`fopen(temp,"wb")`；sha 開始。
- CHUNK：offset/id 檢查；`fwrite`；`psa_hash_update`；ACK。
- END：`psa_hash_finish`；sha 不符 → 刪 temp、`err_sha_mismatch`；符合 → 舊檔 rename `.bak`、temp rename 正式；回 0x2006（pending = .bak 存在）。
- READ：任何模式可讀（app 或 `/ro` 唯讀視圖）；最多 4096。
- LIST：`opendir/readdir`，cJSON 砌 `{"path":...,"entries":[{"n","d","s"}],"more":0}`；上限 ~2 KB。
- DELETE：移除檔＋`.bak`；CONFIRM：移除 `.bak`；UNDO：刪新檔、`.bak` 改名返。
- 全部回覆用 `send_payload()`（現有）。
- **唔改** `nc_store` 所有權邏輯；FAT 唔係 app → 直接 `err_busy`。

- [ ] **Step 1: 加 session／mutex／路徑 helper**
- [ ] **Step 2: BEGIN/CHUNK/END/ACK**
- [ ] **Step 3: READ/CONFIRM/DELETE/UNDO**
- [ ] **Step 4: FILE_LIST/RSP**
- [ ] **Step 5: 更新 dispatch table＋commands.md**
- [ ] **Step 6: 人手 review（冇 IDF）：檢查 includes、API signature、stack/heap 用量**

**Compile 驗收（喺有 IDF 嘅機）：**
```
python make.py build s3 --tests
python make.py build p4
```
Expected: 0 warning / 0 error。

---

## Task 8: 文件、launch、收尾

**Files:**
- Create: `tools/esp_webmaster/README.md`
- Create: `tools/esp_webmaster/launch.py`
- Create: `docs/03_notes/file-transfer.md`
- Modify: `tools/README.md`

- [ ] **Step 1: README**（啟動、sim、連線、限制、協議表）
- [ ] **Step 2: launch.py**（venv、pip install -r、`run.py --open`）
- [ ] **Step 3: 文件**（file-transfer 設計、pending/.bak 語義、FAT 擁有權）
- [ ] **Step 4: 全量測試**

Run: `cd tools/esp_webmaster && python -m pytest -q`
Expected: 全綠

- [ ] **Step 5: 用 sim 起 server＋瀏覽器截圖，檢查 5 個 tab 都通**

---

## 驗收清單

- [ ] `python tools/esp_webmaster/run.py --sim` 開到 UI
- [ ] 檔案 tab：入 folder、上傳（拖拉）、下載、編輯儲存、刪除、確認、復原
- [ ] 固件 tab：拖 .bin、進度、完成
- [ ] Console：0x1101 有 JSON 回覆
- [ ] 終端：見到 log 行＋操作事件
- [ ] Firmware source 有齊 0x2001–0x200A、0x2012/0x2013
- [ ] `commands.md` 同 `file-transfer.md` 更新
- [ ] 冇改動 mp_Net-Core

## 已知限制（要寫入 README）

1. 本機冇 IDF，firmware 未 compile；要喺有 IDF 嘅機 build。
2. CDC 一次一個 client（COM port 獨佔）；WebMaster 舊工具唔可以同時開。
3. 上傳係 4 KB chunk＋逐包 ACK，速度受 CDC 同 FAT 寫入限制（預估 100–300 KB/s）。
4. 檔案寫入需要裝置擁有 FAT（app mode）；MSC 掛住時會 `err_busy`。
5. 冇 mid-file resume（partial）；斷線要由頭再上（v1 範圍）。
6. SD 卡（`/sd`）v1 只讀，唔支援寫入。
