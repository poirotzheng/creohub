@echo off
rem =====================================================================
rem creo-hub plugin_test_kit acceptance runner (M3.5 F4, Q-M3.5-4=A).
rem   test\run_plugin_selftest.cmd <package-dir>
rem [1/5] read package id from plugin.json
rem [2/5] compile offline selftest (cmd-module contract layer, no PTK run)
rem [3/5] run selftest assertions
rem [4/5] manifest_lint on the package (copy-flow isolated root)
rem [5/5] D-W7 on bin\<id>.dll (skipped while still the lint placeholder)
rem Then prints the acceptance-chain checklist (mount/smoke/audit/R2).
rem Exit: 0 = ALL PASS, 1 = FAIL, 2 = tooling/precondition error
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

if "%~1"=="" (
    echo [ERR] usage: run_plugin_selftest.cmd ^<package-dir^>
    popd
    exit /b 2
)
set "PKG=%~f1"
if not exist "%PKG%\plugin.json" (
    echo [ERR] plugin.json not found under %PKG%
    popd
    exit /b 2
)

for /f "usebackq delims=" %%i in (`node -e "console.log(JSON.parse(require('fs').readFileSync(process.argv[1],'utf8')).id)" "%PKG%\plugin.json"`) do set "ID=%%i"
if "%ID%"=="" (
    echo [ERR] cannot read package id
    popd
    exit /b 2
)
set "CI=%ID:-=_%"

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
if not exist "%PTK%\includes\ProToolkit.h" (
    echo [ERR] PTK SDK not found: %PTK%
    exit /b 2
)

echo [1/5] package=%PKG% id=%ID%

echo [2/5] compile offline selftest
if not exist "%OUT%\selftest" mkdir "%OUT%\selftest"
cl /nologo /O2 /W3 /MT /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub\vendor\cjson /Isdk /I"%PKG%\src" /I"%PKG%\test" "%PKG%\test\%ID%_selftest.c" "%PKG%\src\%CI%_main.c" "%PKG%\src\%CI%_cmds.c" sdk\hub_sdk.c src\hub\vendor\cjson\cJSON.c /Fo%OUT%\selftest\ /Fe%OUT%\selftest\%ID%_selftest.exe /link /MACHINE:X64 /SUBSYSTEM:CONSOLE /LIBPATH:"%PTK%\x86e_win64\obj" protk_dll_NU.lib ucore.lib udata.lib netapi32.lib mpr.lib ws2_32.lib psapi.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib winspool.lib bcrypt.lib
if errorlevel 1 (
    echo === PLUGIN SELFTEST: COMPILE FAIL ===
    popd
    exit /b 1
)

echo [3/5] run selftest assertions
rem PTK runtime (ucore46.dll) is provided by Creo at DLL load time; for the
rem offline exe we put the PTK runtime dir on PATH first.
set "PTKRUNTIME=%PTK%\..\x86e_win64\lib"
set "PATH=%PTKRUNTIME%;%PATH%"
%OUT%\selftest\%ID%_selftest.exe
set "EC=%ERRORLEVEL%"
if not "%EC%"=="0" (
    echo === PLUGIN SELFTEST: ASSERTIONS FAIL exit %EC% ===
    popd
    exit /b 1
)

echo [4/5] manifest_lint (copy-flow)
if exist "%OUT%\selfroot" rmdir /s /q "%OUT%\selfroot"
mkdir "%OUT%\selfroot\%ID%" 2>nul
xcopy /e /i /q /y "%PKG%" "%OUT%\selfroot\%ID%" >nul
node test\manifest_lint.mjs %OUT%\selfroot
if errorlevel 1 (
    echo === PLUGIN SELFTEST: LINT FAIL ===
    popd
    exit /b 1
)

echo [5/5] D-W7 self-check
for %%A in ("%PKG%\bin\%ID%.dll") do set "DLLSZ=%%~zA"
if "%DLLSZ%"=="0" (
    echo [SKIP] bin\%ID%.dll is still the lint placeholder - compile the DLL first
) else (
    call test\w7_check.cmd "%PKG%\bin\%ID%.dll"
    if errorlevel 1 (
        echo === PLUGIN SELFTEST: D-W7 FAIL ===
        popd
        exit /b 1
    )
)

echo === PLUGIN SELFTEST: ALL PASS ===
echo [acceptance chain reminder]
echo   1. node test\manifest_lint.mjs plugins  (already done above)
echo   2. panel: Refresh -^> Mount -^> %ID%.status smoke call
echo   3. confirm/destructive commands: 428 -^> panel approve -^> same-args retry
echo   4. audit pairing (txn across confirm_approve -^> call)
echo   5. Unmount -^> R2 terminate: clean
popd
exit /b 0
