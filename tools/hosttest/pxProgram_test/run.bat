@echo off
REM ===========================================================================
REM  pxProgram_test - DATA plane program interpreter (segments -> per-frame values).
REM
REM   run.bat            build + run (auto pick cl / gcc)
REM   run.bat cl|gcc     pick compiler
REM
REM  Expected values come from the MicroPython version (mp_Net-Core/slave),
REM  not from the C code's own interpretation.
REM ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0\..\..\.."

set "PXF=firmware\components\sw\pixel"
set "T=tools\hosttest\pxProgram_test"
set "OUT=%T%\build"
set "SRCS=%PXF%\asset\nc_px_json.c %PXF%\asset\nc_px_asset.c %PXF%\engine\nc_px_engine_tables.c %PXF%\engine\nc_px_engine_program.c %T%\pxProgram_test.c"

if not exist "%OUT%" mkdir "%OUT%"

if "%1"=="gcc" goto gcc
if "%1"=="cl" goto cl
where cl >nul 2>&1 && goto cl
goto gcc

:cl
echo [pxProgram_test] MSVC
cl /nologo /utf-8 /std:c17 /experimental:c11atomics /W3 /I%PXF%\include /I%T%\include /I%T% /Itools\hosttest\pxEngine_test\include /Fe:"%OUT%\pxProgram_test.exe" /Fo:"%OUT%\\" %SRCS%
if errorlevel 1 exit /b 1
"%OUT%\pxProgram_test.exe"
exit /b %ERRORLEVEL%

:gcc
echo [pxProgram_test] gcc
gcc -std=c17 -Wall -Wextra -Werror -O1 -I%PXF%\include -I%T%\include -I%T% -Itools\hosttest\pxEngine_test\include -o "%OUT%\pxProgram_test.exe" %SRCS% -lm
if errorlevel 1 exit /b 1
"%OUT%\pxProgram_test.exe"
exit /b %ERRORLEVEL%
