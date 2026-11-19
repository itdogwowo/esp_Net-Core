@echo off
REM ===========================================================================
REM  pixel_test — 渲染器核心的 host 測試（環 + 編碼器；唔需要 ESP-IDF）
REM
REM   run.bat           建置並跑（預設找 cl / gcc）
REM   run.bat gcc       指定編譯器（cl 或 gcc）
REM
REM  驗：環語義（滿了不覆蓋 / 假消費 / 重用 / 換場）、
REM      編碼位元（GRB / GRBW / SPI / APA102）、抖動平均、多 lane 合成、
REM      mock / direct 後端走同一份 codec。
REM ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0\..\..\.."

set "PXF=firmware\components\sw\pixel"
set "PXT=firmware\components\tasks\render_task"
set "INC=/I%PXF%\include /I%PXT%\include /Itools\hosttest\pixel_test\include"
set "SRCS=%PXF%\nc_pixel_q.c %PXF%\nc_pixel_encode.c %PXT%\nc_pixel_backend_mock.c %PXT%\nc_pixel_backend_direct.c tools\hosttest\pixel_test\pixel_test.c"

if "%1"=="gcc" goto gcc
if "%1"=="cl" goto cl

where cl >nul 2>&1 && goto cl
goto gcc

:cl
echo [pixel_test] MSVC
if not exist "%~dp0build" mkdir "%~dp0build"
cl /nologo /utf-8 /std:c17 /experimental:c11atomics /W3 /WX %INC% /Fe:"%~dp0build\pixel_test.exe" /Fo:"%~dp0build\\" %SRCS%
if errorlevel 1 exit /b 1
"%~dp0build\pixel_test.exe"
exit /b %ERRORLEVEL%

:gcc
echo [pixel_test] gcc
"%PXF%" >nul 2>&1
gcc -std=c17 -D_DEFAULT_SOURCE -Wall -Wextra -Wno-unused-parameter -I%PXF%\include -I%PXT%\include -Itools\hosttest\pixel_test\include -o "%~dp0build\pixel_test.exe" %SRCS%
if errorlevel 1 exit /b 1
"%~dp0build\pixel_test.exe"
exit /b %ERRORLEVEL%
