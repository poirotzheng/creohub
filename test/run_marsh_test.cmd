@echo off
rem =====================================================================
rem creo-hub M2 OFFLINE marshaling+token test runner (gates B-1/B-2:
rem E6 window pump paths + G4 token lifecycle + web_api integration).
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

echo [1/2] compiling marsh_test.exe
cl /nologo /O2 /W3 /MD /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub /Isrc\hub\vendor\cjson /Isdk test\marsh_test.c test\test_stubs.c src\hub\httpd.c src\hub\web_api.c src\hub\jobs.c src\hub\call_exec.c src\hub\schema_val.c src\hub\nonce.c src\hub\opmode.c src\hub\version_mgr.c src\hub\hub_marsh.c src\hub\hub_token.c src\hub\registry.c src\hub\state.c src\hub\reconcile.c src\hub\ledger.c src\hub\vendor\cjson\cJSON.c ws2_32.lib advapi32.lib user32.lib bcrypt.lib /Fo%OUT%\ /Fe%OUT%\marsh_test.exe
if errorlevel 1 exit /b 2

echo [2/2] running marsh_test.exe
echo.
%OUT%\marsh_test.exe
set "RC=%ERRORLEVEL%"
echo.
echo test exit code: %RC%
popd
exit /b %RC%
