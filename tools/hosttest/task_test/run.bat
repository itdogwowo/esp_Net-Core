@echo off
rem task manager host test (Windows / MSVC)
setlocal enabledelayedexpansion
cd /d "%~dp0..\..\.."

where cl >nul 2>&1
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
  if defined VSDIR call "!VSDIR!\VC\Auxiliary\Build\vcvars64.bat" >nul
)

if not exist "%TEMP%\task_test_obj\" mkdir "%TEMP%\task_test_obj"

cl /nologo /utf-8 /std:c11 /W4 ^
  /I tools\hosttest\task_test\include ^
  /I firmware\components\sys\task\include ^
  /I firmware\components\sys\task ^
  /I firmware\components\sys\task_manager ^
  /I firmware\components\sys\config\include ^
  /I firmware\components\sys\nc4\include ^
  /I firmware\components\sys\mem\include ^
  /I firmware\components\sys\epoch\include ^
  firmware\components\sys\task_manager\nc_task.c ^
  firmware\components\sys\epoch\nc_epoch.c ^
  tools\hosttest\task_test\cfg_stub.c ^
  tools\hosttest\task_test\stubs.c ^
  tools\hosttest\task_test\main.c ^
  /Fe:"%TEMP%\task_test.exe" /Fo:"%TEMP%\task_test_obj\\"
if errorlevel 1 exit /b 1
"%TEMP%\task_test.exe"
