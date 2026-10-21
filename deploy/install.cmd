@echo off
rem =====================================================================
rem creo-hub installer kernel (M4c C1, docs\39 6.1/6.2) - NO Node required
rem
rem   install.cmd [--root <dir>] [--creo-home <dir>] [--src <dir>]
rem               [--mode full^|core] [--silent] [--yes] [--log <file>]
rem
rem Actions (each idempotent):
rem   1) preflight  : source payload present, target writable
rem   2) copy       : build\out + web + hub_text + plugins  (robocopy)
rem   3) register   : write <root>\creo-hub.dat + marker block in config.pro
rem                   (marker block is removable by uninstall.cmd)
rem   4) verify     : key files exist
rem   5) next steps : how to start hub inside Creo
rem
rem Exit: 0 ok, 1 preflight fail, 2 copy fail, 3 register fail
rem NOTE: keep this file CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ME=%~f0"
set "SRC=%~dp0.."
set "ROOT="
set "CREO_HOME="
set "MODE=full"
set "SILENT="
set "YES="
set "LOG="

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--root"      ( set "ROOT=%~2"      & shift & shift & goto :parse )
if /i "%~1"=="--creo-home" ( set "CREO_HOME=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--src"       ( set "SRC=%~2"       & shift & shift & goto :parse )
if /i "%~1"=="--mode"      ( set "MODE=%~2"      & shift & shift & goto :parse )
if /i "%~1"=="--log"       ( set "LOG=%~2"       & shift & shift & goto :parse )
if /i "%~1"=="--silent"    ( set "SILENT=1" & shift & goto :parse )
if /i "%~1"=="--yes"       ( set "YES=1"    & shift & goto :parse )
echo [ERR] unknown argument: %~1
exit /b 1
:parsed

if not defined ROOT set "ROOT=%SRC%"
for %%I in ("%SRC%") do set "SRC=%%~fI"
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if defined CREO_HOME for %%I in ("%CREO_HOME%") do set "CREO_HOME=%%~fI"

call :say "[creo-hub install] src=%SRC%"
call :say "[creo-hub install] root=%ROOT% mode=%MODE%"

rem ---------------- 1) preflight ----------------
if not exist "%SRC%\build\out\creo_hub.dll" (
    call :say "[FAIL] payload missing: %SRC%\build\out\creo_hub.dll"
    exit /b 1
)
if not exist "%SRC%\web\index.html" (
    call :say "[FAIL] payload missing: %SRC%\web\index.html"
    exit /b 1
)
if not exist "%ROOT%" mkdir "%ROOT%" 2>nul
call :probe_write "%ROOT%"
if errorlevel 1 (
    call :say "[FAIL] target not writable: %ROOT%"
    exit /b 1
)
if defined CREO_HOME if not exist "%CREO_HOME%" (
    call :say "[FAIL] --creo-home does not exist: %CREO_HOME%"
    exit /b 1
)
call :say "[1/5] preflight OK"

rem ---------------- 2) copy ----------------
if /i "%SRC%"=="%ROOT%" goto :copy_skip
call :robocopy "%SRC%\build\out" "%ROOT%\build\out" || exit /b 2
call :robocopy "%SRC%\web"       "%ROOT%\web"       || exit /b 2
if exist "%SRC%\hub_text" call :robocopy "%SRC%\hub_text" "%ROOT%\hub_text" || exit /b 2
if exist "%SRC%\plugins"  call :robocopy "%SRC%\plugins"  "%ROOT%\plugins"  || exit /b 2
call :say "[2/5] payload copied"
goto :register
:copy_skip
call :say "[2/5] payload already in place (src==root; Inno shell copied it)"

:register
rem ---------------- 3) register ----------------
> "%ROOT%\creo-hub.dat" (
    echo name creo_hub
    echo startup dll
    echo exec_file %ROOT%\build\out\creo_hub.dll
    echo text_dir %ROOT%\hub_text
    echo description creo-hub plugin manager M4
    echo fail_tol TRUE
    echo allow_stop TRUE
    echo revision Creo6.0
    echo end
)
if not exist "%ROOT%\creo-hub.dat" (
    call :say "[FAIL] cannot write %ROOT%\creo-hub.dat"
    exit /b 3
)
if not defined CREO_HOME (
    call :say "[3/5] registry file written; config.pro NOT touched (no --creo-home)"
    call :say "      register manually: add 'PROTKDAT %ROOT%\creo-hub.dat' to the config.pro"
    call :say "      in Creo's STARTUP directory (shortcut 'Start in', often C:\Users\Public\Documents)"
    goto :verify
)
set "CFG=%CREO_HOME%\config.pro"
if exist "%CFG%" (
    if not exist "%CFG%.creo-hub-bak" copy /y "%CFG%" "%CFG%.creo-hub-bak" >nul
    rem remove previous marker block (idempotent re-install)
    findstr /v /c:"# >>> creo-hub" /c:"# <<< creo-hub" /c:"creo-hub.dat" "%CFG%" > "%CFG%.creo-hub-tmp"
    if exist "%CFG%.creo-hub-tmp" move /y "%CFG%.creo-hub-tmp" "%CFG%" >nul
)
rem F2 field-proven (2026-10-18, Creo 6.0 + config.pro in the startup dir):
rem keyword PROTKDAT + an independent .dat registers the app and auto-starts it.
>> "%CFG%" echo # ^>^>^> creo-hub (managed by install.cmd; do not edit) ^>^>^> 
>> "%CFG%" echo PROTKDAT %ROOT%\creo-hub.dat
>> "%CFG%" echo # ^<^<^< creo-hub ^<^<^< 
findstr /c:"creo-hub.dat" "%CFG%" >nul
if errorlevel 1 (
    call :say "[FAIL] registration marker not written to %CFG%"
    exit /b 3
)
call :say "[3/5] registered: %ROOT%\creo-hub.dat + marker in %CFG%"

:verify
rem ---------------- 4) verify ----------------
if not exist "%ROOT%\build\out\creo_hub.dll" ( call :say "[FAIL] verify: creo_hub.dll" & exit /b 2 )
if not exist "%ROOT%\web\index.html"           ( call :say "[FAIL] verify: web\index.html" & exit /b 2 )
call :say "[4/5] verify OK"

rem ---------------- 5) next steps ----------------
call :say "[5/5] done. Next:"
call :say "      1) start Creo - creo_hub loads AUTOMATICALLY (no Aux Apps step needed)"
call :say "      2) open the panel: Creo menu Utilities - creo-hub - Open Web Panel"
call :say "      3) optional assistant: agent\start_agent.cmd (configure model in the panel)"
call :say "      4) run doctor.cmd for a health report"
if defined SILENT exit /b 0
exit /b 0

rem ---------------------------------------------------------------- helpers
:say
echo %~1
if defined LOG echo %~1>>"%LOG%"
exit /b 0

:probe_write
set "PW=%TEMP%\creo_hub_probe_%RANDOM%.tmp"
> "%PW%" echo probe 2>nul
if not exist "%PW%" exit /b 1
del "%PW%" >nul 2>nul
exit /b 0

:robocopy
robocopy "%~1" "%~2" /e /njh /njs /ndl /nfl /r:1 /w:1 >nul
if errorlevel 8 exit /b 1
exit /b 0
