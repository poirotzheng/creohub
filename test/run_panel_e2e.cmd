@echo off
rem =====================================================================
rem creo-hub M2c panel E2E runner (gate O7+ static half + E8 offline part)
rem   [1/6] compile api_serve.exe (full stack servant, auto-exit)
rem   [2/6] node static check: ES5 purity + zero external refs
rem   [3/6] headless Chrome renders the panel against the live stack
rem   [4/6] node dump assertions (rendered DOM)
rem   [5/6] node contract assertions (live API shape the panel codes to)
rem   [6/6] wait for servant auto-exit (no orphan pipe handles)
rem Exit code: 0 = ALL PASS, 1 = failures, 2 = compile error, 3 = no browser
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

set "TOKEN=0f1e2d3c4b5a69788796a5b4c3d2e1f0"
set "PORT=18800"

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

echo [1/6] compiling api_serve.exe
cl /nologo /O2 /W3 /MD /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub /Isrc\hub\vendor\cjson /Isdk test\api_serve.c test\test_stubs.c src\hub\httpd.c src\hub\web_api.c src\hub\jobs.c src\hub\call_exec.c src\hub\schema_val.c src\hub\nonce.c src\hub\opmode.c src\hub\version_mgr.c src\hub\registry.c src\hub\state.c src\hub\reconcile.c src\hub\ledger.c src\hub\vendor\cjson\cJSON.c ws2_32.lib advapi32.lib bcrypt.lib /Fo%OUT%\ /Fe%OUT%\api_serve.exe
if errorlevel 1 exit /b 2

echo [2/6] static panel check (ES5 + air-gap)
node test\panel_check.mjs static "%ROOT%\web\index.html"
if errorlevel 1 exit /b 1

set "BROWSER="
if exist "%ProgramFiles%\Google\Chrome\Application\chrome.exe" set "BROWSER=%ProgramFiles%\Google\Chrome\Application\chrome.exe"
if not defined BROWSER if exist "%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe" set "BROWSER=%ProgramFiles(x86)%\Microsoft\Edge\Application\msedge.exe"
if not defined BROWSER if exist "%ProgramFiles%\Microsoft\Edge\Application\msedge.exe" set "BROWSER=%ProgramFiles%\Microsoft\Edge\Application\msedge.exe"
if defined BROWSER goto :browser_ok
echo [ERR] no Chrome/Edge found for headless render
exit /b 3
:browser_ok

echo [3/6] starting servant + headless render (%BROWSER%)
start "" /B %OUT%\api_serve.exe %PORT% 60 %TOKEN% "%ROOT%\web\index.html"
ping -n 4 127.0.0.1 >nul
"%BROWSER%" --headless=new --no-sandbox --disable-gpu --disable-crash-reporter --no-first-run --no-default-browser-check --user-data-dir="%TEMP%\creo_hub_chrome" --virtual-time-budget=12000 --dump-dom "http://127.0.0.1:%PORT%/?token=%TOKEN%" > %OUT%\dump.html 2> %OUT%\chrome_err.txt
echo chrome exit code: %ERRORLEVEL%

echo [4/6] dump assertions
node test\panel_check.mjs dump %OUT%\dump.html %TOKEN%
if errorlevel 1 goto :fail_after_serve

echo [5/6] contract assertions
node test\panel_check.mjs contract %PORT% %TOKEN%
if errorlevel 1 goto :fail_after_serve

echo [6/6] waiting for servant auto-exit
:waitserve
tasklist /FI "IMAGENAME eq api_serve.exe" 2>nul | findstr /I "api_serve.exe" >nul
if errorlevel 1 goto :servedone
ping -n 3 127.0.0.1 >nul
goto :waitserve
:servedone
echo === PANEL E2E: ALL PHASES PASS ===
popd
exit /b 0

:fail_after_serve
taskkill /IM api_serve.exe /F >nul 2>&1
popd
exit /b 1
