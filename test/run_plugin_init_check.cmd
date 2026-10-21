@echo off
rem =====================================================================
rem creo-hub plugin_init scaffold self-check gate (M3.5 F3).
rem [1/6] non-interactive generate -> %OUT%\init_check\initpkg
rem       (1 safe stub-free status + 3 stubs incl. args/destructive paths)
rem [1b/6] requires/provides generation (M4a A4) + negative path
rem [2/6] compile generated six-module skeleton -> initpkg.dll (Win7 flags)
rem [3/6] copy-flow manifest_lint on the generated package
rem [4/6] D-W7 self-check on the generated DLL
rem [5/6] plugin_test_kit selftest on the generated package
rem [6/6] dependency-declaration package: lint + selftest
rem Exit: 0 = ALL PASS, 1 = FAIL, 2 = tooling/precondition error
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
if not exist "%PTK%\includes\ProToolkit.h" (
    echo [ERR] PTK SDK not found: %PTK%
    exit /b 2
)

echo [1/6] generate skeleton (non-interactive)
if exist "%OUT%\init_check" rmdir /s /q "%OUT%\init_check"
node tools\plugin_init\plugin_init.mjs %OUT%\init_check\initpkg --id initpkg --name "Init SelfCheck" --cmd "probe:safe;set_val:confirm:val:number;flag_it:destructive:on:boolean" --non-interactive --skip-lint
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: GENERATE FAIL ===
    popd
    exit /b 1
)

echo [1b/6] requires/provides generation (M4a A4)
if exist "%OUT%\init_req" rmdir /s /q "%OUT%\init_req"
node tools\plugin_init\plugin_init.mjs %OUT%\init_req --id initreq --name "Init Requires" --cmd "probe:safe" --requires demo --provides initreq.probe --non-interactive --skip-lint
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: GENERATE REQ FAIL ===
    popd
    exit /b 1
)
node -e "const path=require('path');const j=require(path.resolve(process.env.OUT,'init_req','plugin.json')); if(!j.requires||j.requires.length!==1||j.requires[0]!=='demo'||!j.provides||j.provides[0]!=='initreq.probe') process.exit(1);"
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: REQ/PROVIDES FIELDS FAIL ===
    popd
    exit /b 1
)
node test\manifest_lint.mjs %OUT%\init_req
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: REQ LINT FAIL ===
    popd
    exit /b 1
)
if exist "%OUT%\init_bad" rmdir /s /q "%OUT%\init_bad"
node tools\plugin_init\plugin_init.mjs %OUT%\init_bad --id initbad --cmd "probe:safe" --requires Bad_ID --non-interactive --skip-lint >nul 2>nul
if not errorlevel 1 (
    echo === PLUGIN INIT CHECK: BAD REQUIRES NOT REJECTED ===
    popd
    exit /b 1
)

echo [2/6] compile generated skeleton
if not exist "%OUT%\init_check\initpkg\bin" mkdir "%OUT%\init_check\initpkg\bin"
if not exist "%OUT%\init_out" mkdir "%OUT%\init_out"
cl /nologo /LD /O2 /W3 /MT /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /D_USRDLL /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub\vendor\cjson /Isdk /I%OUT%\init_check\initpkg\src %OUT%\init_check\initpkg\src\initpkg_main.c %OUT%\init_check\initpkg\src\initpkg_cmds.c %OUT%\init_check\initpkg\src\initpkg_adapter.c sdk\hub_sdk.c src\hub\vendor\cjson\cJSON.c /Fo%OUT%\init_out\ /Fe%OUT%\init_check\initpkg\bin\initpkg.dll /link /MACHINE:X64 /SUBSYSTEM:WINDOWS /DEF:%OUT%\init_check\initpkg\initpkg.def /LIBPATH:"%PTK%\x86e_win64\obj" protk_dll_NU.lib ucore.lib udata.lib netapi32.lib mpr.lib ws2_32.lib psapi.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib winspool.lib bcrypt.lib
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: COMPILE FAIL ===
    popd
    exit /b 1
)

echo [3/6] copy-flow manifest lint
if exist "%OUT%\initroot" rmdir /s /q "%OUT%\initroot"
mkdir "%OUT%\initroot\initpkg" 2>nul
xcopy /e /i /q /y "%OUT%\init_check\initpkg" "%OUT%\initroot\initpkg" >nul
node test\manifest_lint.mjs %OUT%\initroot
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: LINT FAIL ===
    popd
    exit /b 1
)

echo [4/6] D-W7 self-check on generated DLL
call test\w7_check.cmd %OUT%\init_check\initpkg\bin\initpkg.dll
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: D-W7 FAIL ===
    popd
    exit /b 1
)

echo [5/6] plugin_test_kit selftest on generated package
call test\run_plugin_selftest.cmd %OUT%\init_check\initpkg
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: SELFTEST FAIL ===
    popd
    exit /b 1
)

echo [6/6] dependency-declaration package: selftest
call test\run_plugin_selftest.cmd %OUT%\init_req
if errorlevel 1 (
    echo === PLUGIN INIT CHECK: REQ SELFTEST FAIL ===
    popd
    exit /b 1
)

echo === PLUGIN INIT CHECK: ALL PASS ===
popd
exit /b 0
