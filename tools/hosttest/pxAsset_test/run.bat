@echo off
REM ===========================================================================
REM  pxAsset_test - DATA plane asset parsing (JSON -> struct) + directory scan.
REM
REM   run.bat            build + run (auto pick cl / gcc)
REM   run.bat cl|gcc     pick compiler
REM
REM  Creates and removes .\pxAsset_tmp\ while running.
REM  Windows needs the dirent shim: include\win\dirent.h (POSIX build uses the
REM  real one, so include\win is only added here).
REM ===========================================================================
setlocal EnableExtensions
cd /d "%~dp0\..\..\.."

set "PXF=firmware\components\sw\pixel"
set "T=tools\hosttest\pxAsset_test"
set "OUT=%T%\build"
set "SRCS=%PXF%\asset\nc_px_asset.c %PXF%\asset\nc_px_asset_scan.c %T%\pxAsset_test.c"

if not exist "%OUT%" mkdir "%OUT%"

if "%1"=="gcc" goto gcc
if "%1"=="cl" goto cl
where cl >nul 2>&1 && goto cl
goto gcc

:cl
echo [pxAsset_test] MSVC
cl /nologo /utf-8 /std:c17 /experimental:c11atomics /W3 /I%PXF%\include /I%T%\include /I%T%\include\win /I%T% /Fe:"%OUT%\pxAsset_test.exe" /Fo:"%OUT%\\" %SRCS%
if errorlevel 1 exit /b 1
"%OUT%\pxAsset_test.exe"
exit /b %ERRORLEVEL%

:gcc
echo [pxAsset_test] gcc
gcc -std=c17 -Wall -Wextra -Werror -O1 -I%PXF%\include -I%T%\include -I%T% -o "%OUT%\pxAsset_test.exe" %SRCS%
if errorlevel 1 exit /b 1
"%OUT%\pxAsset_test.exe"
exit /b %ERRORLEVEL%
