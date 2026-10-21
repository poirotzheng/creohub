@echo off
setlocal
rem ============================================================
rem  creo-hub M3c bridge offline battery runner (docs\23 s4.5)
rem  Double-click OK. [1/2] compile api_serve.exe (callfw-capable
rem  firmware, same toolchain flags as run_panel_e2e.cmd) [2/2]
rem  run the REAL MCP client battery (groups A..J + H security).
rem  No Creo, no internet, no user DSH_HOME involved.
rem  ENV NOTE (M3.5 F7): group G (token/hub-down semantics) EXPECTS the
rem  hub to be DOWN; running against a live hub yields exactly one
rem  explainable red (G4 - killServer leaves the stub while the bridge
rem  talks to the live hub). Full green baseline = hub stopped (108/0).
rem  NOTE: this file MUST stay CRLF + pure ASCII.
rem ============================================================
cd /d "%~dp0.."
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

where node >nul 2>&1
if errorlevel 1 goto NONODE

if not exist "bridge\node_modules\@modelcontextprotocol\sdk" goto NOSDK

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

echo [1/2] compiling api_serve.exe (call stubs + callfw fixture mode)
cl /nologo /O2 /W3 /MD /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub /Isrc\hub\vendor\cjson /Isdk test\api_serve.c test\test_stubs.c src\hub\httpd.c src\hub\web_api.c src\hub\jobs.c src\hub\call_exec.c src\hub\schema_val.c src\hub\nonce.c src\hub\opmode.c src\hub\version_mgr.c src\hub\registry.c src\hub\state.c src\hub\reconcile.c src\hub\ledger.c src\hub\vendor\cjson\cJSON.c ws2_32.lib advapi32.lib bcrypt.lib /Fo%OUT%\ /Fe%OUT%\api_serve.exe
if errorlevel 1 exit /b 2

echo [2/2] battery (real MCP client -^> bridge.mjs -^> api_serve callfw fixture)
node "bridge\test\bridge_test.mjs"
set RC=%errorlevel%
if "%RC%"=="0" goto PASS

echo.
echo [battery] FAILED rc=%RC% - see [FAIL] lines above
goto END

:PASS
echo.
echo [battery] ALL PASS - M3c offline gate green
goto END

:NONODE
echo [ERR] node not found in PATH - install Node 18+ or fix PATH
set RC=2
goto END

:NOSDK
echo [ERR] MCP SDK missing - run: cd bridge ^&^& npm install --cache %USERPROFILE%\.npm-cache
set RC=2
goto END

:END
echo.
echo exit code: %RC%
if "%1"=="" if not defined CI pause
exit /b %RC%