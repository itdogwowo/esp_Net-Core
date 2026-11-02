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

cl /nologo /utf-8 /std:c11 /W4 ^
  /I tools\hosttest\task_test\include ^
  /I firmware\components\sys\tasks\include ^
  /I firmware\components\sys\tasks ^
  /I firmware\components\sys\nc4\include ^
  /I firmware\components\sys\mem\include ^
  firmware\components\sys\tasks\nc_task.c ^
  tools\hosttest\task_test\stubs.c ^
  tools\hosttest\task_test\main.c ^
  /Fe:"%TEMP%\task_test.exe" /Fo:"%TEMP%\\"
if errorlevel 1 exit /b 1
"%TEMP%\task_test.exe"
