<#
  ESP WebMaster — Windows 啟動（PowerShell 版，對應同一個資料夾嘅 web.sh）

  位置：repo 根 → web_tools/web.ps1
    repo/
    ├─ make.py / build.sh / make.bat     建置（同網頁工具無關）
    └─ web_tools/
       ├─ settings.json                  基礎數值（port／host／autoconnect…）
       ├─ settings.local.json            只係我部機（唔入 git，選用）
       ├─ web.sh / WebMaster.command     macOS／Linux
       ├─ web.ps1 / WebMaster.bat        Windows（本檔）
       └─ webmaster/                     server + 前端（run.py／static）

  Windows 10／11 都可以用；PowerShell 5.1 同 7 都跑得。唔使改 ExecutionPolicy：
    powershell -NoProfile -ExecutionPolicy Bypass -File .\web_tools\web.ps1
  或者直接雙擊 web_tools\WebMaster.bat（佢會用同一個方式叫呢個檔，
  並且用 `py -3` 揀 Python）。

  同 web.sh 嘅差異（刻意嘅）：
  - web.sh 預設「只開 server＋自動開瀏覽器」；Windows 雙擊時預設係**模擬裝置**
    （免硬件、免接線、第一次用就有畫面）——要真板請加 --serial COM13。
  - 有 --serial 時預設自動開瀏覽器；同時加 --no-open 可以關。

  用法（喺 repo 根）：
    .\web_tools\web.ps1                        模擬裝置 + 開瀏覽器
    .\web_tools\web.ps1 -Serial COM13          真板（S3 原生 USB CDC）
    .\web_tools\web.ps1 -Serial COM13 -Port 8011
    .\web_tools\web.ps1 -Port 8020 -NoOpen
    .\web_tools\web.ps1 -AutoPort              8010 被佔用就自動揀下一個
    .\web_tools\web.ps1 -Fresh                  刪 .venv 重裝（deps 壞咗時用）
    .\web_tools\web.ps1 -Sim -SimDir .\web_tools\webmaster\.simfs
#>
[CmdletBinding()]
param(
  [switch]$Sim,
  [string]$Serial,
  [string]$SimDir,
  [int]$Port = 8010,
  [string]$HostAddr = '127.0.0.1',
  [switch]$NoOpen,
  [switch]$AutoPort,
  [switch]$Fresh
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$base = Join-Path $root 'webmaster'
$venv = Join-Path $base '.venv'
$venvPy = Join-Path $venv 'Scripts\python.exe'

function Write-Step($msg) { Write-Host "[WebMaster] $msg" -ForegroundColor Cyan }
function Write-Warn2($msg) { Write-Host "[WebMaster] $msg" -ForegroundColor Yellow }
function Write-Err2($msg) { Write-Host "[WebMaster] $msg" -ForegroundColor Red }

# ── 0. PowerShell 5.1 防呆：呢個腳本唔需要任何新語法，但 UTF-8 輸出要設好 ──
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch { }

# ── 1. repo 檢查 ───────────────────────────────────────────────────────────
if (-not (Test-Path (Join-Path $base 'run.py'))) {
  Write-Err2 "搵唔到 web_tools\webmaster\run.py — 確認 web_tools\ 完整（webmaster\ 冇被搬走）。"
  exit 2
}

# ── 2. 揀 Python ──────────────────────────────────────────────────────────
# 用 `--version` 探測（唔用 `-c "..."`）：Windows PowerShell 5.1 傳參數俾原生程式
# 時，含 `"` 的 `-c` 字串會被拆爛（實測 SyntaxError）；`--version` 冇引號，穩。
function Find-Python {
  $cands = @(
    @{ Exe = 'py';      Args = @('-3') },
    @{ Exe = 'python';  Args = @()     },
    @{ Exe = 'python3'; Args = @()     }
  )
  foreach ($c in $cands) {
    $cmd = Get-Command $c.Exe -ErrorAction SilentlyContinue
    if (-not $cmd) { continue }
    try {
      $out = (& $c.Exe @($c.Args + @('--version')) 2>&1 | Out-String).Trim()
    } catch {
      continue   # Microsoft Store 的 python3 捷徑：未裝就 throw，跳下一個
    }
    if ($LASTEXITCODE -ne 0 -or -not $out) { continue }
    $m = [regex]::Match($out, 'Python\s+(\d+)\.(\d+)')
    if (-not $m.Success) { continue }
    $major = [int]$m.Groups[1].Value
    $minor = [int]$m.Groups[2].Value
    if ($major -lt 3 -or ($major -eq 3 -and $minor -lt 9)) {
      Write-Warn2 "略過 $($c.Exe)：Python $major.$minor 太舊（要 3.9+）"
      continue
    }
    return @{ Exe = $c.Exe; Args = $c.Args; Version = "$major.$minor" }
  }
  return $null
}

$py = Find-Python
if (-not $py) {
  Write-Err2 '搵唔到 Python 3。請裝： https://www.python.org/downloads/windows/ （要勾 Add python.exe to PATH）'
  exit 3
}
$pyExe = $py.Exe
$pyArgs = @($py.Args)          # e.g. @('-3')（py launcher）或 @()
Write-Step "Python $($py.Version)（$($py.Exe)）"

# ── 3. venv（.venv／requirements 變過就重裝）──────────────────────────────
if ($Fresh -and (Test-Path $venv)) {
  Write-Step '刪除舊 venv（-Fresh）…'
  Remove-Item -Recurse -Force $venv
}

$needVenv = $true
if (Test-Path $venvPy) {
  # deps 檢查：`-c` 內文**唔可以有雙引號**（PS 5.1 傳原生參數時會拆爛）。
  try {
    $null = & $venvPy @('-c', 'import fastapi, uvicorn, serial') 2>&1
    if ($LASTEXITCODE -eq 0) { $needVenv = $false }
  } catch { $needVenv = $true }
}

if ($needVenv) {
  if (-not (Test-Path $venvPy)) {
    Write-Step '建立 venv（web_tools\webmaster\.venv）…'
    Push-Location $base
    try { & $pyExe @($pyArgs + @('-m', 'venv', $venv)) } finally { Pop-Location }
    if (-not (Test-Path $venvPy)) { Write-Err2 '建立 venv 失敗。'; exit 4 }
  }
  Write-Step '安裝 dependencies（第一次要 1–2 分鐘，之後會快取）…'
  # --no-cache-dir：本機 pip 快取目錄可能被 ACL 鎖住（WinError 5 / 「存取被拒」），
  # 寫入 wheel 快取會整個 install 失敗；唔用快取就繞過去，代價只係 -Fresh 時再下載。
  & $venvPy @('-m', 'pip', 'install', '-q', '--no-cache-dir',
              '--disable-pip-version-check',
              '-r', (Join-Path $base 'requirements.txt'))
  if ($LASTEXITCODE -ne 0) { Write-Err2 'pip install 失敗（試下 .\web_tools\web.ps1 -Fresh，或者檢查網絡）。'; exit 5 }
}

# ── 4. 組 run.py 參數 ────────────────────────────────────────────────────
# 重要：呢個檔**唔再自己決定 port／host／開唔開瀏覽器**——全部交俾
# webmaster\settings.json（＋命令行覆寫）。以前 PS 自己預設 8010、Python 又一個
# 預設，兩邊一有分歧就會出現「明明改咗設定檔但仍然報 8010 被佔用」。
# 所以：只有用戶**明確**俾參數時才加落去；否則一個都唔加，由 run.py 讀設定檔。
$runArgs = @((Join-Path $base 'run.py'))

if ($PSBoundParameters.ContainsKey('Port')) { $runArgs += @('--port', "$Port") }
if ($PSBoundParameters.ContainsKey('HostAddr')) { $runArgs += @('--host', $HostAddr) }
if ($SimDir) { $runArgs += @('--sim-dir', $SimDir) }

if ($Sim.IsPresent) {
  $runArgs += '--sim'
  Write-Step '模擬裝置模式（虛擬 FAT，唔使板）'
} elseif ($Serial) {
  $runArgs += @('--serial', $Serial)
  Write-Step "連線序列埠 $Serial"
} else {
  Write-Step '冇指定 -Serial → 連線目標跟 settings.json 的 device.autoconnect'
}

if ($NoOpen) { $runArgs += '--no-open' }
if ($AutoPort) { $runArgs += '--auto-port' }

# ── 5. 預覽實際設定（run.py --print-config，同一個真相）──────────────────
Push-Location $base
try {
  $preview = (& $venvPy @(@('run.py') + @('--print-config')) 2>&1 | Out-String)
  if ($LASTEXITCODE -eq 0 -and $preview) {
    try {
      $cfgObj = ($preview | ConvertFrom-Json).effective
      $cfgPort = $cfgObj.server.port
      $cfgHost = $cfgObj.server.host
      Write-Step "設定檔（web_tools\settings.json）：port=$cfgPort host=$cfgHost auto_port=$($cfgObj.server.auto_port) " +
                 "autoconnect=$($cfgObj.device.autoconnect) open=$($cfgObj.server.open_browser)"
    } catch {
      Write-Warn2 '（設定檔預覽解析失敗，照樣啟動）'
    }
  } else {
    Write-Warn2 $preview.Trim()
  }

  # ── 6. 啟動 ───────────────────────────────────────────────────────────
  Write-Step '啟動中（Ctrl+C 停止）…'
  Write-Host ''
  & $venvPy @runArgs
  $rc = $LASTEXITCODE
} finally {
  Pop-Location
}

if ($rc -and $rc -ne 0) {
  Write-Err2 "server 結束（代碼 $rc）。"
  exit $rc
}
Write-Step '已停止。'
exit 0
