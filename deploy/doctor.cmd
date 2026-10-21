@echo off
rem =====================================================================
rem creo-hub doctor (M4c C1, docs\39 6.4) - NO Node required
rem
rem   doctor.cmd [--root <dir>] [--creo-home <dir>] [--mode full^|core]
rem              [--deep] [--report <file>] [--silent]
rem
rem Checks (FAIL=exit 1, WARN=informational):
rem   [1] creo_hub.dll present          [2] web panel present
rem   [3] hub_text present              [4] creo-hub.dat present
rem   [5] config.pro marker             [6] hub_token.txt (informational)
rem   [7] Creo install detected (reg)    [8] node (mode full)
rem   [9] port 8765-8775 (--deep only)  [10] config.pro backup
rem
rem Report: --report <file>, default "<root>\doctor_report.txt"
rem NOTE: keep this file CRLF + pure ASCII.
rem =====================================================================
setlocal enabledelayedexpansion
set "ROOT="
set "CREO_HOME="
set "MODE=full"
set "DEEP="
set "REPORT="
set "SILENT="

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--root"      ( set "ROOT=%~2"      & shift & shift & goto :parse )
if /i "%~1"=="--creo-home" ( set "CREO_HOME=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--mode"      ( set "MODE=%~2"      & shift & shift & goto :parse )
if /i "%~1"=="--report"    ( set "REPORT=%~2"    & shift & shift & goto :parse )
if /i "%~1"=="--deep"      ( set "DEEP=1" & shift & goto :parse )
if /i "%~1"=="--silent"    ( set "SILENT=1" & shift & goto :parse )
echo [ERR] unknown argument: %~1
exit /b 2
:parsed

if not defined ROOT (
    echo [ERR] --root is required
    exit /b 2
)
for %%I in ("%ROOT%") do set "ROOT=%%~fI"
if defined CREO_HOME for %%I in ("%CREO_HOME%") do set "CREO_HOME=%%~fI"
if not defined REPORT set "REPORT=%ROOT%\doctor_report.txt"

set "FAILN=0"
set "WARNN=0"
set "LINES="

> "%REPORT%" echo creo-hub doctor report
>> "%REPORT%" echo root=%ROOT%
>> "%REPORT%" echo creo-home=%CREO_HOME%
>> "%REPORT%" echo mode=%MODE%
>> "%REPORT%" echo date=%DATE% %TIME%
>> "%REPORT%" echo ----------------------------------------

call :check "%ROOT%\build\out\creo_hub.dll" "creo_hub.dll present"
call :check "%ROOT%\web\index.html"           "web panel present"
call :checkdir "%ROOT%\hub_text"              "hub_text directory present"
call :check "%ROOT%\creo-hub.dat"             "creo-hub.dat registration file"
if defined CREO_HOME (
    findstr /c:"creo-hub.dat" "%CREO_HOME%\config.pro" >nul 2>nul
    if errorlevel 1 ( call :fail "config.pro marker present" ) else ( call :ok "config.pro marker present" )
) else (
    call :warn "config.pro marker (no --creo-home given)"
)
if exist "%ROOT%\hub_token.txt" ( call :ok "hub_token.txt present (hub started at least once)" ) else ( call :warn "hub_token.txt absent (hub not started yet)" )
reg query "HKLM\SOFTWARE\PTC" >nul 2>nul
if errorlevel 1 ( reg query "HKCU\SOFTWARE\PTC" >nul 2>nul )
if errorlevel 1 ( call :warn "Creo registry key not found (HKLM/HKCU \SOFTWARE\PTC)" ) else ( call :ok "Creo install detected (registry)" )
if /i "%MODE%"=="full" (
    node --version >nul 2>nul
    if errorlevel 1 ( call :fail "node not found on PATH (mode=full)" ) else ( call :ok "node present (mode=full)" )
) else (
    call :warn "node not required (mode=core)"
)
if exist "%CREO_HOME%\config.pro.creo-hub-bak" ( call :ok "config.pro backup present" ) else ( call :warn "config.pro backup absent" )
if defined DEEP call :portscan

>> "%REPORT%" echo ----------------------------------------
>> "%REPORT%" echo RESULT: %FAILN% fail, %WARNN% warn
if not defined SILENT echo.
if not defined SILENT echo doctor: %FAILN% fail, %WARNN% warn  (report: %REPORT%)
if "%FAILN%"=="0" exit /b 0
exit /b 1

rem ---------------------------------------------------------------- helpers
:ok
set "LINES=%LINES%[OK]   %~1" & >> "%REPORT%" echo [OK]   %~1
if not defined SILENT echo [OK]   %~1
exit /b 0

:warn
set /a WARNN+=1
>> "%REPORT%" echo [WARN] %~1
if not defined SILENT echo [WARN] %~1
exit /b 0

:fail
set /a FAILN+=1
>> "%REPORT%" echo [FAIL] %~1
if not defined SILENT echo [FAIL] %~1
exit /b 0

:check
if exist "%~1" ( call :ok "%~2" ) else ( call :fail "%~2" )
exit /b 0

:checkdir
if exist "%~1\" ( call :ok "%~2" ) else ( call :fail "%~2" )
exit /b 0

:portscan
set "FOUNDPORT="
for /l %%P in (8765,1,8775) do (
    netstat -ano | findstr ":%%P " >nul 2>nul
    if not errorlevel 1 set "FOUNDPORT=%%P"
)
if defined FOUNDPORT ( call :ok "hub listening on 127.0.0.1:%FOUNDPORT%" ) else ( call :warn "no hub listener in 8765-8775 (hub not running)" )
exit /b 0
