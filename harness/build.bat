@echo off
rem Build the reference harness. MUST be 32-bit: ECI.DLL and ENU.SYN are 32-bit.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined VCINSTALLDIR call :findvs
cd /d %~dp0
if not exist ..\build mkdir ..\build
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fo..\build\ ecisay.c user32.lib /Fe:..\build\ecisay.exe || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fo..\build\ fxtest.c ..\src\fx80.c /Fe:..\build\fxtest.exe || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fo..\build\ ecitrace.c user32.lib /Fe:..\build\ecitrace.exe || exit /b 1
cl /nologo /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fo..\build\ mathcheck.c ..\src\x87math.c ..\src\fx80.c /Fe:..\build\mathcheck.exe || exit /b 1
goto :eof

:findvs
for %%d in ("%VSWHERE%") do set "PATH=%%~dpd;%PATH%"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
goto :eof
