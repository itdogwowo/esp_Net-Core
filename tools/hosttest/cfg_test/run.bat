@echo off
rem nc_cfg_patch host test (Windows / MSVC)
setlocal enabledelayedexpansion
cd /d "%~dp0..\..\.."

where cl >nul 2>&1
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
  if defined VSDIR call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

cl /nologo /utf-8 /std:c11 /W4 /WX ^
  /I firmware\components\sys\config\include ^
  /I firmware\components\sys\config ^
  tools\hosttest\cfg_test\main.c ^
  firmware\components\sys\config\nc_cfg_patch.c ^
  /Fe:"%TEMP%\cfg_test.exe" /Fo:"%TEMP%\\"
if errorlevel 1 exit /b 1
"%TEMP%\cfg_test.exe"
