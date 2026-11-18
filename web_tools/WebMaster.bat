@echo off
REM ===========================================================================
REM  ESP WebMaster - Windows one-click launcher (double-click to start)
REM
REM  Location: repo root -> web_tools\WebMaster.bat
REM    repo/
REM      make.py / build.sh / make.bat     build toolchain (unrelated)
REM      web_tools/
REM        web.sh / WebMaster.command      macOS/Linux
REM        web.ps1 / WebMaster.bat         Windows (this file)
REM        webmaster/                      server + front-end
REM
REM  Usage (double-click = default; or from the repo root):
REM    web_tools\WebMaster.bat                    simulated device (no board)
REM    web_tools\WebMaster.bat --serial COM13     real board (S3 native USB CDC)
REM    web_tools\WebMaster.bat --serial COM13 --port 8011
REM    web_tools\WebMaster.bat --port 8010        pick the HTTP port
REM
REM  This file is a thin shell only: the real logic lives in web.ps1
REM  (the Windows counterpart of web.sh). It picks a Python interpreter and
REM  hands over to PowerShell, so no ExecutionPolicy change is required.
REM
REM  NOTE 1: kept pure ASCII on purpose - cmd.exe decodes .bat files with the
REM          console code page, so UTF-8 Chinese here would be garbled (or
REM          worse, break the parser). All Chinese messages come from web.ps1,
REM          which is UTF-8 with BOM and renders correctly everywhere.
REM  NOTE 2: no `cmd && ( ... )` multi-line blocks - cmd.exe rejects those
REM          ("X was unexpected at this time"). Plain `if errorlevel` only.
REM ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

if not exist "web.ps1" (
  echo [ERROR] web.ps1 not found - it must sit next to WebMaster.bat ^(web_tools^).
  goto :hold
)

REM -- 1. Pick a Python: py launcher first, then python, then python3 --------
set "PY="
py -3 --version >nul 2>&1
if not errorlevel 1 set "PY=py -3"
if not defined PY (
  python --version >nul 2>&1
  if not errorlevel 1 set "PY=python"
)
if not defined PY (
  python3 --version >nul 2>&1
  if not errorlevel 1 set "PY=python3"
)
if not defined PY goto :nopython

REM -- 2. Hand over: -ExecutionPolicy Bypass applies to this process only ----
echo [WebMaster] starting web.ps1 %*
where pwsh >nul 2>&1
if not errorlevel 1 goto :use_pwsh
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0web.ps1" %*
goto :after

:use_pwsh
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0web.ps1" %*

:after
set "RC=%ERRORLEVEL%"
if "%RC%"=="0" goto :eof
echo.
echo [WebMaster] exited ^(code %RC%^).
goto :hold

:nopython
echo.
echo [ERROR] Python 3 not found. Install it first ^(tick "Add python.exe to PATH"^):
echo         https://www.python.org/downloads/windows/
echo         or install "Python 3.12+" from the Microsoft Store.
goto :hold

:hold
echo.
pause
endlocal
exit /b 1
