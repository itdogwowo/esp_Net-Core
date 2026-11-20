@echo off
REM ===========================================================================
REM  pxEngine_test - effect engine host test (no ESP-IDF, no board needed)
REM
REM   run.bat            build + run + compare golden.txt (auto pick cl / gcc)
REM   run.bat cl|gcc     pick compiler
REM   run.bat --update   re-shoot golden (only when the behaviour change is intended)
REM
REM  golden.txt was shot with glibc before the M2 refactor; MSVC reproduces it
REM  byte-for-byte (checked 2026-11-20), so both toolchains can gate on it.
REM ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0\..\..\.."

set "PXF=firmware\components\sw\pixel"
set "FXE=firmware\components\effects\pixel_effects"
set "FXT=tools\hosttest\pxEngine_test"
set "OUT=%FXT%\build"
set "INC=/I%PXF%\include /I%FXE%\include /I%FXT%\include /I%FXT%"
set "SRCS=%PXF%\engine\nc_px_engine_core.c %PXF%\engine\nc_px_engine_tables.c %FXE%\nc_effect_zone1.c %FXE%\nc_effect_breath.c %FXE%\nc_effect_wave.c %FXE%\nc_effect_solid.c %FXE%\nc_effect_ramp.c %FXE%\nc_effect_table.c %FXT%\pxEngine_test.c"

set "UPD="
if "%1"=="--update" set "UPD=1"
if "%1"=="--update" shift

if not exist "%OUT%" mkdir "%OUT%"

if "%1"=="gcc" goto gcc
if "%1"=="cl" goto cl
where cl >nul 2>&1 && goto cl
goto gcc

:cl
echo [pxEngine_test] MSVC
cl /nologo /utf-8 /std:c17 /experimental:c11atomics /O2 /W3 %INC% /Fe:"%OUT%\pxEngine_test.exe" /Fo:"%OUT%\\" %SRCS%
if errorlevel 1 exit /b 1
goto run

:gcc
echo [pxEngine_test] gcc
gcc -std=c17 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Wno-unused-parameter %INC% -o "%OUT%\pxEngine_test.exe" %SRCS% -lm
if errorlevel 1 exit /b 1

:run
"%OUT%\pxEngine_test.exe" > "%OUT%\out.txt" 2>nul
type "%OUT%\out.txt"

if defined UPD (
    copy /y "%OUT%\out.txt" "%FXT%\golden.txt" >nul
    echo golden.txt updated
    exit /b 0
)

fc "%FXT%\golden.txt" "%OUT%\out.txt" >nul
if errorlevel 1 (
    echo FAIL: payload hash differs from golden
    fc "%FXT%\golden.txt" "%OUT%\out.txt"
    exit /b 1
)
echo pxEngine_test OK - golden match
exit /b 0
