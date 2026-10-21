@echo off
rem =====================================================================
rem creo-hub M3b b4 OFFLINE log rotation test (R1, real hub_log.c linked)
rem Exit code: 0 = ALL PASS, 1 = failures, 2 = compile error
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" goto :vswhere_ok
echo [ERR] vswhere not found
exit /b 2
:vswhere_ok

set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not "%VS%"=="" goto :vs_ok
echo [ERR] no Visual Studio with VC tools found
exit /b 2
:vs_ok

call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 2

call "%~dp0..\deploy\local_env.cmd"
if errorlevel 1 exit /b 2

if exist "%OUT%" goto :out_ok
mkdir "%OUT%"
:out_ok
if exist "%OUT%\log" goto :log_ok
mkdir "%OUT%\log"
:log_ok

echo [1/2] compiling log_test.exe
cl /nologo /O2 /W3 /MD /utf-8 /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /I"%PTK%\includes" /Isrc\hub test\log_test.c src\hub\hub_log.c /Fo%OUT%\log\ /Fe%OUT%\log_test.exe
if errorlevel 1 exit /b 2

echo [2/2] running log_test.exe
echo.
%OUT%\log_test.exe
set "RC=%ERRORLEVEL%"
echo.
echo test exit code: %RC%
popd
exit /b %RC%
