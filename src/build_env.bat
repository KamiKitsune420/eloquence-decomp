@echo off
rem Set up the Visual Studio command-line environment for the given architecture (x64 or x86).
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for %%d in ("%VSWHERE%") do set "PATH=%%~dpd;%PATH%"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -latest -products * -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul
