# 測試環境（tenv）：每台機自己的現場紀錄

> **這份是協議，不是紀錄。** 它講「AI／人怎樣知道這台機現在能用甚麼測試」。
> 這台機的實際值在 `.testenv/machine.json`＋`last-probe.json`（**不進 git**），
> 每次由 `python3 tools/tenv.py` 重新探測。
>
> 相關：[testing.md](testing.md)（測試方法）、[build_and_flash.md](build_and_flash.md)（建置／OTA）、
> `tools/testenv/policy.json`（團隊政策）。

---

## 0. 為什麼需要這一層

同一個 repo 會在 macOS／Windows（`.idf/`）／WSL 之間來回走。同一句「跑測試」，在每台機
意思都不同：

| 會變的東西 | 例 |
|---|---|
| IDF 版本／路徑 | 同一個 `~/esp/esp-idf` 可以係 v6.0.3，也可以係 v6.1.1；版本不同 = 不同 image，數字不能互比 |
| 有冇板 | 主機可能只跑 host test；WSL 可能完全冇 USB passthrough |
| 板上的 image | 可能係 release／`--tests`／EffectPerf，三種支援的 `0xF0xx` 完全不同 |
| 板上的 task | `/data/config/tasks.json` 可能關掉了 pixel 任務；換 project 後更常見 |
| 本機 artifacts | `artifacts/` 不進 git；這台機最新 build 可能是另一塊板 |

靠「文件寫死版本」或「上次的印象」一定會出事：文件會過期，而且三台機的現實不同步。
**所以：政策放 git，現場事實每次重探，意圖放本機不進 git。**

```text
tools/testenv/policy.json        團隊契約（進 git）
   IDF 要 v6.1.1、host test 21 支、probe 參數
                │
                ▼
.testenv/                        這台機（不進 git；預設 <repo>/.testenv）
   machine.json                  「意圖」：常接哪塊板、cid、project、IDF pin、關掉哪條 lane
   last-probe.json               「事實」：上次探到的 IDF／port／板／image 能力
                │
                ▼
python3 tools/tenv.py brief      AI 每次開工讀這一行命令的輸出
   → facts + policy + intent → plan：runnable / blocked / unknown / disabled
```

**不變式**：`machine.json` 只記「想用甚麼」；能不能跑由 probe 回答。紀錄過期不會令
AI 以為自己測過，只會令 plan 顯示 drift，叫你再探一次。

---

## 1. 一次性設定（每台機 2 分鐘）

```bash
# 1) 建立本機紀錄（會自動探一次）
python3 tools/tenv.py init --label "macOS 主力"

# 2) 綁定這台機常接的板（board 名用 ports/hardware 的真相）
#    有板時最好用 bind：它會順便把板上的 cid 記下來
python3 tools/tenv.py bind ESP32-S3-Zero --project ESP32-S3-Zero-Show9600

#    板不在手邊，也可以先寫意圖（cid 之後再補）
python3 tools/tenv.py set board ESP32-S3-Zero
python3 tools/tenv.py set project ESP32-S3-Zero-Show9600
python3 tools/tenv.py set cid 2B4C

# 3) 如果這台機固定用某個 IDF（不想被 IDF_PATH 影響）
python3 tools/tenv.py set idf ~/esp/esp-idf

# 4) 主機型機器（WSL／CI，永遠不接板）
python3 tools/tenv.py set absent-ok true
```

- `bind` 一定要板在線；`set` 不需要。`bind` 會驗證 board／project 存在（讀 `ports/`）。
- 接錯板時 plan 會出 `board_cid` warning：cid 和 `machine.json` 不同。
- 每台機各自一份；不要把 `.testenv/` 複製去另一台（會被 machine id 檢查攔住）。

### 放在哪裡？（重要）

預設 `＜repo＞/.testenv/`，已加進 `.gitignore`：**不會經 git 同步**，`git clean -xdf` 會清掉
（清掉只失去意圖，重新 `init`＋`bind` 即可；事實本來每次重探）。

但有兩種情況要用 repo 外的目錄：

1. **整個 repo 放在 iCloud／Dropbox／OneDrive**：那個資料夾本身會被同步，`.testenv/`
   也會跟着走。改用：
   ```bash
   python3 tools/tenv.py init --state-dir ~/.esp_net_core/testenv
   # 之後每個命令都帶同一個 --state-dir，或設一次環境變數：
   export ESP_NET_CORE_TESTENV_DIR=~/.esp_net_core/testenv
   ```
2. **Windows／WSL 共用同一個 repo**：兩邊的 home 不同，預設 `.testenv/` 會是同一份
   （如果 repo 在 WSL 的 `/mnt/c/...`），machine id 會對不上。兩邊各自用
   `--state-dir`（例如 WSL：`~/.esp_net_core/testenv`）。

`tenv where` 會印出這次實際用的位置。

---

## 2. 每次開工（AI 必做）

```bash
python3 tools/tenv.py brief
```

輸出是一頁 Markdown：機器、repo、IDF、板、image、每條 lane 的狀態。**在這之後才決定
要跑甚麼**。需要細節用：

```bash
python3 tools/tenv.py plan -v     # 每條 lane 的前置、指令、閘門、文件
python3 tools/tenv.py plan --json # 給程式／AI 讀（lane 狀態機讀）
python3 tools/tenv.py verify      # 紀錄和現實有冇漂移（不寫檔，exit 2 = 有）
```

三條規矩：

1. **`blocked`／`unknown` 不等於測過。** 冇板就寫「未上板」，不要用 host 數字假裝板上數字。
2. **回報任何數字前貼 stamp**（見 §5）。跨 IDF 版本的數字不可互相對比。
3. **plan 是判斷，不是行動。** 它不會 build／flash／OTA；`runnable` 只是「前置齊了」。

### 狀態的意義

| 狀態 | 意思 | AI 應該怎樣講 |
|---|---|---|
| `runnable` | 前置都齊，可以照 `command` 跑 | 「可以跑，閘門是 …」 |
| `blocked` | 前置不齊（冇板／冇 IDF／image 不對） | 「這台機未做，原因是 …」——不可以寫「測過」 |
| `unknown` | 探測不足（有 port 但冇回覆、`status cmds` 讀不到） | 「不確定，先重試／檢查線」——不可以當 blocked 或通過 |
| `disabled` | `machine.json` 明確關掉（absent_ok／lanes.disabled） | 「這台機設計上不跑」 |

`unknown` 和 `blocked` 分開是故意的：線鬆、port 被 monitor 佔住，重試會變 runnable；
「冇 IDF」重試一百次都不會變。兩者的處理不同。

---

## 3. Lane 一覽（工具判斷的依據）

| lane | 需要 | 主要指令 | 通過閘門 |
|---|---|---|---|
| `host` | bash＋C 編譯器 | `bash tools/hosttest/run_all.sh` | 21/21 全綠（`-Werror`） |
| `build` | IDF＋`machine.board.expected` | `IDF_PATH=… python3 make.py build <board> [--project …]` | rc=0；artifacts 的 `.json`／`.bin` sha256 一致 |
| `board` | 序列埠＋NC4 回覆 | `nc4.py --port <p> status sys` | sys／tasks／mem 有回覆；`rst` 不是 4／5／6 |
| `pixel` | 板＋四個 pixel 任務 | `nc4.py --port <p> status tasks` | 四任務 `level=4`、`started=1`、`running=1` |
| `ota` | 板＋對應 image | `ota.py --port <p> <bin>` | APPLY 後 `app` 變、`rst=3`；中途不拔線 |
| `tests` | 板＋image 有 `0xF061` | `nc4.py raw 0xF061 00` | 回 `producer`／`render` JSON，不是 err |
| `effect-perf` | 板＋image 有 `0xF065` | `effect_bench.py --port <p>` | 每組 px 有數字；同 image 再跑對照 |
| `ncm` | 板＋image 有 `ncm` 段 | `nc4.py --port <p> status ncm` | `backend=wamr`、`ok:1`、`frames` 增長 |

板上的能力**不是靠 artifacts 檔名猜**，是問 image 自己：

- `status cmds` 有 `0xF061` → `NC_ENABLE_TESTS` 映像；
- 有 `0xF065` → EffectPerf 映像；
- `status ncm` 有 `ncm` 段 → NCM／WAMR 有編進去；
- `status tasks` 四個 pixel 任務都在、`level=4` → 這是會點燈的 image。

所以同一塊板即使 image 是在**另一台電腦** build／OTA 的（本機 `artifacts/` 找不到），
tenv 仍然答得出它支援甚麼。

---

## 4. `machine.json`：意圖，不是事實

`tenv show`／`tenv where` 可以看到實際檔案。形狀（省略部分欄位）：

```json
{
  "schema": 1,
  "machine": {"id": "macos-arm64-…", "label": "macOS 主力", "role": "dev"},
  "idf":    {"pin": "/Users/…/esp/esp-idf"},
  "board":  {"expected": "ESP32-S3-Zero", "expected_cid": "2B4C",
             "project": "ESP32-S3-Zero-Show9600", "absent_ok": false},
  "lanes":  {"disabled": []},
  "notes": ""
}
```

- `machine.id` 由硬件指紋（macOS `IOPlatformUUID`／Linux `/etc/machine-id`／Windows
  `MachineGuid`）得出，不是 hostname、也不是 `uuid.getnode()`（後者在拿不到 MAC 時
  **每次隨機**，會令每個命令都以為換了機）。
- `board.expected` 只用來決定 build／OTA 的目標，以及接錯板時 warning；它不是「現在
  接着的板」——現在接着甚麼由 probe 回答。
- `board.absent_ok=true` 是給 WSL／CI 的：沒有板不是錯誤，板類 lane 直接 `disabled`。
- `lanes.disabled` 是機器層的例外，例如這台機不准 OTA：
  `tenv lane disable ota`。

改意圖用 `tenv set …`（會驗證 board／project／IDF 路徑），不要手改 `last-probe.json`
（下次 plan 會覆蓋）。

### 政策檔（進 git）

`tools/testenv/policy.json` 只放三種東西：IDF 要求、host test 預期支數、probe 參數。
**要改「三個環境要同一個 IDF 版本」只改這裡一處**；`AGENTS.md` 與本文件引用它。

如果本機 IDF 與政策不同，plan 會出 `idf_policy` warning。這不是故障：本機 A/B 仍然
有效，但**跨機數字不可比**；記錄要寫 `idf=`。要消除 warning 只有兩條路：三台一起
升／降 IDF，或改政策（並在 commit message 講為什麼）。

---

## 5. Stamp：貼進測試紀錄的一行

```bash
python3 tools/tenv.py stamp
# env=macOS_主力 idf=v6.0.3 dev_idf=v6.0.3 board=ESP32-S3-Zero cid=2B4C \
# fw=8afc0a4 app=1a2b3c4d img=release git=8afc0a4
```

| 欄位 | 意思 |
|---|---|
| `env` | 這台機的 label（空白換成 `_`） |
| `idf` | 主機 build 用的 IDF 版本 |
| `dev_idf` | 板上 image 自己報的 IDF 版本（`status sys.idf`） |
| `board`／`cid` | 現在接着的板（不是 `expected`） |
| `fw`／`app` | 板上 `status sys` 的韌體版本與 image 前三 bytes |
| `img` | `release`／`tests`／`effect-perf`／`tests+effect`／`unknown` |
| `git` | 工作區 rev（不是板上） |

規矩（跟鐵律 12）：

- 記錄任何 build／量測／OTA 都要有 `idf=` 與環境；沒有就不可以和其他數字比較。
- A/B 一定同一塊板、同一 image、同一 `idf`。
- 跨機比較之前先看兩邊 stamp 的 `idf`／`board`／`img` 是否一致。

---

## 6. 常見情況

| 症狀 | 意思 | 處理 |
|---|---|---|
| `board` lane `blocked: 冇序列埠` | 板未接／線／driver | 接板；`python3 make.py ports`；再 `tenv plan` |
| `board` lane `unknown: 冇 STATUS_RSP` | port 有，但 app 未起／port 被佔／USJ vs CDC | 關 monitor／WebMaster；換 port；等 2 秒再試 |
| `board_cid` warning | 接着的板 cid 與紀錄不同 | 接錯板，或接受新板：`tenv set cid <新 cid>` |
| `idf_policy` warning | 本機 IDF ≠ 政策 | 不要跨機比數字；記錄寫 `idf=`；要統一就三台一起換 |
| `host` lane `blocked: 冇 bash` | Windows 沒 Git Bash／WSL | 裝 Git Bash／用 WSL；或逐支跑 `run.bat` |
| Windows 主機照 `build` lane 的命令跑一定失敗 | PATH 上的 `bash` 是 **WSL bash**（或 Git Bash），bash 收 `C:\…` 會當成檔名；`doctor` 的 board check 會**假報**「0 個錯誤」的 FAIL | 建置／host test 改在 WSL 內跑：`bash -lc 'cd /mnt/c/<repo> && IDF_PATH=/home/user/esp/esp-idf python3 make.py build …'`；OTA／NC4／board 留在 Windows（`python3 tools/…`＋`COM*`）。詳見 `AGENTS.md` §0.5 |
| `board` lane `unknown: PermissionError` | 另一個程式（WebMaster／monitor）持有 COM port | 關掉它，或 `POST /api/port/release`；測完 `POST /api/port/reclaim` |
| `pixel` lane `blocked: 任務未齊` | `/data/config/tasks.json` 舊／換 project | `fs_format` 重 seed（鐵律 6）再 reboot |
| `tests` lane `blocked: 冇 0xF061` | 板上是 release／EffectPerf image | 用 `--tests` build 再 OTA；EffectPerf 用 `0xF065` |
| `state dir 屬於另一台機` | 同步了 `.testenv/` 或兩個 OS 共用 repo | 用 `--state-dir` 指去本機專用目錄；確定要覆蓋才 `init --force` |
| `verify` 出 `repo_moved` | 上次 probe 之後有 commit | 正常；跑一次 `tenv plan` 接受新現況 |

### 不可以做的事

- 不要 commit `.testenv/`（內含主機路徑與板 cid）。
- 不要手改 `last-probe.json` 假裝有板／有 tests image；plan 只信 probe。
- 不要用 `blocked`／`unknown` 的 lane 數字寫報告。
- 不要把 `.testenv/` 當成團隊文件；它是**這台機**的現場，換機就重新 init。

---

## 7. 設計理由（為什麼不是別的做法）

| 做法 | 為什麼不做 |
|---|---|
| 把環境寫進 `make_config.json`／`AGENTS.md` | 會進 git，三台機互相覆蓋；而且改一次要 commit 一次，最後一定過期 |
| 只信 `artifacts/` 最新 build | artifacts 不進 git；板上 image 可能來自另一台機，本機 artifacts 對不上 |
| 用 MAC／`uuid.getnode()` 做機器 id | macOS 拿不到 MAC 時 Python 每次隨機生成（multicast bit=1），每個命令都變「另一台機」（2026-11-26 實際踩到） |
| 「有 port 就當有板」 | port 可以是 Bluetooth／debug console；Windows 更會有主機板內建 COM |
| 自動幫你 build／OTA | 判斷和行動要分開；plan 只講可行性，實際命令仍然由人／AI 逐條跑，出事才知道是哪一步 |

非目標：tenv 不做測試排程、不取代 `make.py doctor`（doctor 檢查工具鏈是否完整；
tenv 回答「這台機現在能用哪種測試」），也不上傳任何東西。

---

## 8. 現況快照（2026-11-26，首次建立）

- 這台 macOS 探到 `~/esp/esp-idf` = **v6.0.3**；政策寫 **v6.1.1** → plan 會出
  `idf_policy` warning。本機另有 v5.4.1／v5.5.5 兩個 IDF，工具會列出但不採用。
- 最新本機 build：`ESP32-S3-Zero__ESP32-S3-Zero-Show9600`（tests=false，idf=v6.0.3）。
- 建立紀錄時沒有板在線（`/dev/cu.usbmodem*` 為空），所以板類 lane 全部 `blocked`。
- 以上是快照；**以 `tenv brief` 的即時輸出為準**。
