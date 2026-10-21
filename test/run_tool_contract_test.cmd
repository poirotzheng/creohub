@echo off
setlocal
rem ============================================================
rem  creo-hub M3c tool-face CONTRACT TEST runner (DoD D10,
rem  docs\23 s4.5). Double-click OK. [1/2] compile api_serve.exe
rem  (callfw firmware) [2/2] three-way drift check: live manifest
rem  <-> lib\tool_gen.mjs projection <-> committed golden JSON.
rem  Pass "update" as arg 1 to regenerate the golden snapshot.
rem  NOTE: this file MUST stay CRLF + pure ASCII.
rem ============================================================
cd /d "%~dp0.."
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

where node >nul 2>&1
if errorlevel 1 goto NONODE

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

echo [1/2] compiling api_serve.exe (callfw fixture firmware)
cl /nologo /O2 /W3 /MD /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub /Isrc\hub\vendor\cjson /Isdk test\api_serve.c test\test_stubs.c src\hub\httpd.c src\hub\web_api.c src\hub\jobs.c src\hub\call_exec.c src\hub\schema_val.c src\hub\nonce.c src\hub\opmode.c src\hub\version_mgr.c src\hub\registry.c src\hub\state.c src\hub\reconcile.c src\hub\ledger.c src\hub\vendor\cjson\cJSON.c ws2_32.lib advapi32.lib bcrypt.lib /Fo%OUT%\ /Fe%OUT%\api_serve.exe
if errorlevel 1 exit /b 2

set "GFLAG="
if /i "%~1"=="update" set "GFLAG=--update-golden"
echo [2/2] contract test (D10: live manifest ^<-^> tool_gen ^<-^> golden)
node "test\tool_contract_test.mjs" %GFLAG%
set RC=%errorlevel%
if "%RC%"=="0" goto PASS

echo.
echo [contract] FAILED rc=%RC% - see [FAIL] lines above
goto END

:PASS
echo.
echo [contract] ALL PASS - tool face matches the manifest contract
goto END

:NONODE
echo [ERR] node not found in PATH - install Node 18+ or fix PATH
set RC=2
goto END

:END
echo.
echo exit code: %RC%
if "%1"=="" if not defined CI pause
exit /b %RC%
