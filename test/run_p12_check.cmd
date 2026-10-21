@echo off
rem =====================================================================
rem creo-hub P12 artifact check: feed test\p12_demo_plugin_v2.json (the
rem REAL file the user swaps in during docs\17 P12) through the REAL
rem registry scanner + R2 badge flow. Exit code = failures.
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

cl /nologo /O2 /W3 /MD /utf-8 /D_WINDOWS /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub /Isrc\hub\vendor\cjson test\p12_check.c test\test_stubs.c src\hub\registry.c src\hub\state.c src\hub\ledger.c src\hub\nonce.c src\hub\version_mgr.c src\hub\vendor\cjson\cJSON.c bcrypt.lib /Fo%OUT%\ /Fe%OUT%\p12_check.exe
if errorlevel 1 exit /b 2

%OUT%\p12_check.exe test\p12_demo_plugin_v2.json
set RC=%ERRORLEVEL%
popd
exit /b %RC%
