@echo off
rem =====================================================================
rem creo-hub M2 OFFLINE httpd test runner (gates O3 + O8 httpd layer).
rem Phase 1: compile + run in-process real-socket matrix (httpd_test.exe).
rem Phase 2: start --serve instance, hit it with an EXTERNAL Node client
rem          (test\httpd_smoke.mjs), then kill the serve instance.
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

echo [1/3] compiling httpd_test.exe
cl /nologo /O2 /W3 /MD /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS /I"%PTK%\includes" /Isrc\hub test\httpd_test.c test\test_stubs.c src\hub\httpd.c ws2_32.lib /Fo%OUT%\ /Fe%OUT%\httpd_test.exe
if errorlevel 1 exit /b 2

echo [2/3] running httpd_test.exe (O3 matrix + O8 fuzz)
echo.
%OUT%\httpd_test.exe
set "RC=%ERRORLEVEL%"
echo.
echo phase-1 exit code: %RC%
if not "%RC%"=="0" goto :done

echo [3/3] external Node client smoke (real second client, real port)
where node >nul 2>nul
if errorlevel 1 goto :no_node
start "" /B %OUT%\httpd_test.exe --serve 18766 30
ping -n 3 127.0.0.1 >nul
node test\httpd_smoke.mjs 18766
set "RC=%ERRORLEVEL%"
taskkill /F /IM httpd_test.exe >nul 2>nul
echo phase-2 exit code: %RC%
goto :done
:no_node
echo [WARN] node not found - external smoke skipped (phase 1 already covers O3 with real sockets)

:done
popd
exit /b %RC%
