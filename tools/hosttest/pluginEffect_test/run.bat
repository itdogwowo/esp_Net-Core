@echo off
rem 插件工具 x 效果 .nce 的 host 測試（Windows / MSVC）
setlocal enabledelayedexpansion
cd /d "%~dp0..\..\.."

where cl >nul 2>&1
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
  if defined VSDIR call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

cl /nologo /utf-8 /std:c11 /W4 /D_CRT_SECURE_NO_WARNINGS /DNC_MOD_HOST_TEST ^
  /I tools\hosttest\hostshim ^
  /I firmware\components\host\ncm\include ^
  /I firmware\components\sw\plugin\include ^
  firmware\components\host\ncm\nc_mod.c ^
  firmware\components\host\ncm\nc_res.c ^
  firmware\components\sw\plugin\nc_plugin.c ^
  firmware\components\sw\plugin\nc_ncd.c ^
  firmware\components\sw\plugin\nc_meta.c ^
  tools\hosttest\hostshim\mod_host.c ^
  tools\hosttest\pluginEffect_test\main.c ^
  /Fe:"%TEMP%\pluginEffect_test.exe" /Fo:"%TEMP%\\"
if errorlevel 1 exit /b 1
"%TEMP%\pluginEffect_test.exe"
