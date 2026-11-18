@echo off
REM ===========================================================================
REM  ESP Net-Core - build / flash helper for Windows (same as: python make.py)
REM
REM  Platform map: Windows -> this file; macOS/Linux -> build.sh
REM
REM  Usage (identical to make.py):
REM    make.bat doctor                     environment health check
REM    make.bat boards                     list boards
REM    make.bat build s3                   build (S3)
REM    make.bat build p4 --tests           build (P4 + tests)
REM    make.bat build --all                every board
REM    make.bat flash s3                   flash
REM    make.bat check                      validate config / resources
REM
REM  Implementation lives in tools\ncbuild\; board truth in
REM  ports\<chip>\<board>\board.json. Export ESP-IDF first:
REM  %IDF_PATH%\export.bat
REM
REM  NOTE: pure ASCII on purpose (cmd.exe decodes .bat with the console code
REM  page). Chinese documentation lives in build.sh / make.py / docs.
REM ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0"

set "PY="
py -3 -c "import sys" >nul 2>&1 && set "PY=py -3"
if not defined PY ( python -c "import sys" >nul 2>&1 && set "PY=python" )
if not defined PY ( python3 -c "import sys" >nul 2>&1 && set "PY=python3" )
if not defined PY (
  echo [build] Python 3 not found. Install from https://www.python.org/downloads/windows/
  exit /b 1
)

%PY% "%~dp0make.py" %*
exit /b %ERRORLEVEL%
