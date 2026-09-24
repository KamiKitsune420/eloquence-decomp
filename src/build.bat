@echo off
rem Build the Eloquence port and its test programs.  build.bat [x64|x86]   (default x64)
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCINSTALLDIR call :findvs
cd /d %~dp0
set B=..\build\%ARCH%
if not exist %B% mkdir %B%
set CF=/nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /fp:precise
set RT=x86rt.c fx80.c x87math.c crt.c image.c

if not exist %B%\synth mkdir %B%\synth
cl %CF% /Fo%B%\synth\ %RT% gen_synth.c synth_replay.c /Fe:%B%\synth_replay.exe || exit /b 1

rem the hand-written synthesizer (klatt.c; needs only fx80.c) against the same recordings, and (-diff)
rem against the recompiled original
if not exist %B%\klatt mkdir %B%\klatt
cl %CF% /Fo%B%\klatt\ %RT% gen_synth.c klatt.c klatt_check.c /Fe:%B%\klatt_check.exe || exit /b 1

rem the whole engine, recompiled: gen\enu_*.c (tools\x2c.py --split 24)
if not exist %B%\enu mkdir %B%\enu
cl %CF% /I. /MP4 /c /Fo%B%\enu\ gen\enu_*.c || exit /b 1
rem with the hand-written synthesizer (klatt.c) and frame builder (framer.c) in place of the recompiled ones
cl %CF% /Fo%B%\ %RT% klatt.c klatt_guest.c framer.c framer_guest.c rules.c rules_ops.c rules_delta.c rules_pool.c rules_edit.c rules_guest.c rules_io.c tracks.c eloq_run.c %B%\enu\*.obj /Fe:%B%\eloq_run.exe || exit /b 1
goto :eof

:findvs
for %%d in ("%VSWHERE%") do set "PATH=%%~dpd;%PATH%"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul
goto :eof
