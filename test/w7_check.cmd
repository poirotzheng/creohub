@echo off
rem =====================================================================
rem creo-hub D-W7 gate: Win7-offline static compatibility check (docs\25 ?3).
rem Verifies on build\out\creo_hub.dll (dumpbin):
rem   1) no UCRT forwarding imports (api-ms-win-*) and no ucrtbase  (/MT rule)
rem   2) PE subsystem version <= 6.01 (Win7 loader acceptance)
rem   3) imported DLL set is a subset of the Win7-stock whitelist
rem Exit code: 0 = PASS, 1 = FAIL, 2 = tooling error
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"

set "DLL=build\out\creo_hub.dll"
if not "%~1"=="" set "DLL=%~1"
if exist "%DLL%" goto :dll_ok
echo [ERR] %DLL% not found - run build.cmd first
exit /b 2
:dll_ok

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

set "IMP=%TEMP%\dsw7_imports.txt"
set "HDR=%TEMP%\dsw7_headers.txt"
dumpbin /imports "%DLL%" > "%IMP%" 2>&1
if errorlevel 1 exit /b 2
dumpbin /headers "%DLL%" > "%HDR%" 2>&1
if errorlevel 1 exit /b 2

set /a FAIL=0

rem --- 1) UCRT / api-set imports must be absent (/MT static CRT rule) ---
findstr /i /c:"api-ms-win" /c:"ucrtbase" "%IMP%" >nul 2>&1
if not errorlevel 1 (
    echo [FAIL] UCRT/api-set imports found - /MT rule violated
    set /a FAIL=1
) else (
    echo [OK] no api-ms-win-*/ucrtbase imports
)

rem --- 2) subsystem version must be 6.00 or 6.01 ---
findstr /c:"6.00 subsystem version" /c:"6.01 subsystem version" "%HDR%" >nul 2>&1
if not errorlevel 1 (
    echo [OK] subsystem version Win7-compatible
) else (
    echo [FAIL] subsystem version not 6.00/6.01 - Win7 loader would reject
    findstr /c:"subsystem version" "%HDR%"
    set /a FAIL=1
)

rem --- 3) import whitelist (Win7 stock + Creo); anything else fails ---
rem (no space before the pipe: "echo %%d |" would append a trailing space
rem  and break findstr /x full-line matching. bcrypt.dll reserved for b2.
rem  M3.5 static-MFC set (OLEAUT32/OLEACC/gdiplus/IMM32/WINMM) added after
rem  D-W7=B ruling 2026-10-17: all five are Win7 in-box system DLLs, kept
rem  out of the original list only because the hub never linked MFC.)
for /f "tokens=1" %%d in ('findstr /r /c:"^    [A-Za-z0-9_\-]*\.dll" "%IMP%"') do (
    echo %%d| findstr /i /x "ucore46.dll KERNEL32.dll USER32.dll ADVAPI32.dll SHELL32.dll VERSION.dll WS2_32.dll NETAPI32.dll MPR.dll GDI32.dll ole32.dll winspool.dll bcrypt.dll OLEAUT32.dll OLEACC.dll gdiplus.dll IMM32.dll WINMM.dll MSIMG32.dll COMCTL32.dll SHLWAPI.dll UxTheme.dll" >nul 2>&1
    if errorlevel 1 (
        echo [FAIL] non-whitelisted import: %%d
        set /a FAIL=1
    )
)
if "%FAIL%"=="0" echo [OK] import set within Win7 whitelist

del "%IMP%" "%HDR%" >nul 2>&1
if "%FAIL%"=="0" (
    echo ================ D-W7 PASS ================
    popd
    exit /b 0
)
echo ================ D-W7 FAIL ================
popd
exit /b 1
