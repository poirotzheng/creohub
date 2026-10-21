@echo off
rem =====================================================================
rem creo-hub plugin template compile gate (M3e dev-scheme code-level).
rem [1/3] compile tools\plugin_tpl six-module skeleton -> bin\tpl.dll
rem       (Win7 offline discipline flags, same table as build.cmd v4)
rem [2/3] copy-workflow lint: tpl -> isolated root -> manifest_lint green
rem [3/3] D-W7 self-check on the template DLL (w7_check with arg)
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
if not exist "tools\plugin_tpl\src\tpl_adapter.c" (
    echo [ERR] template missing: tools\plugin_tpl
    exit /b 2
)

echo [1/3] compile template DLL (six modules, Win7 discipline flags)
if not exist "tools\plugin_tpl\bin" mkdir "tools\plugin_tpl\bin"
if not exist "%OUT%\tpl" mkdir "%OUT%\tpl"
cl /nologo /LD /O2 /W3 /MT /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /D_USRDLL /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub\vendor\cjson /Isdk /Itools\plugin_tpl\src tools\plugin_tpl\src\tpl_main.c tools\plugin_tpl\src\tpl_cmds.c tools\plugin_tpl\src\tpl_adapter.c sdk\hub_sdk.c src\hub\vendor\cjson\cJSON.c /Fo%OUT%\tpl\ /Fetools\plugin_tpl\bin\tpl.dll /link /MACHINE:X64 /SUBSYSTEM:WINDOWS /DEF:tools\plugin_tpl\tpl.def /LIBPATH:"%PTK%\x86e_win64\obj" protk_dll_NU.lib ucore.lib udata.lib netapi32.lib mpr.lib ws2_32.lib psapi.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib winspool.lib bcrypt.lib
if errorlevel 1 (
    echo === PLUGIN TPL CHECK: COMPILE FAIL ===
    popd
    exit /b 1
)

echo [2/3] copy-workflow manifest lint (isolated root)
if exist "%OUT%\tplroot" rmdir /s /q "%OUT%\tplroot"
mkdir "%OUT%\tplroot\plugin_tpl" 2>nul
xcopy /e /i /q /y "tools\plugin_tpl" "%OUT%\tplroot\plugin_tpl" >nul
node test\manifest_lint.mjs %OUT%\tplroot
if errorlevel 1 (
    echo === PLUGIN TPL CHECK: LINT FAIL ===
    popd
    exit /b 1
)

echo [3/3] D-W7 self-check on template DLL
call test\w7_check.cmd tools\plugin_tpl\bin\tpl.dll
if errorlevel 1 (
    echo === PLUGIN TPL CHECK: D-W7 FAIL ===
    popd
    exit /b 1
)

echo === PLUGIN TPL CHECK: ALL PASS ===
popd
exit /b 0
