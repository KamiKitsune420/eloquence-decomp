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

rem the whole engine, recompiled: gen\enu_*.c (tools\x2c.py --split 24)
if not exist %B%\enu mkdir %B%\enu
cl %CF% /I. /MP4 /c /Fo%B%\enu\ gen\enu_*.c || exit /b 1
cl %CF% /Fo%B%\ %RT% eloq_run.c %B%\enu\*.obj /Fe:%B%\eloq_run.exe || exit /b 1
goto :eof

:findvs
for %%d in ("%VSWHERE%") do set "PATH=%%~dpd;%PATH%"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul
goto :eof
