@echo off
rem =====================================================================
rem creo-hub uninstaller kernel (M4c C1, docs\39 6.1/6.2) - NO Node required
rem
rem   uninstall.cmd [--root <dir>] [--creo-home <dir>] [--purge] [--silent] [--log <f>]
rem
rem Reverses install.cmd registration ONLY (marker block + creo-hub.dat);
rem payload files are kept unless --purge. config.pro backup is never
rem restored automatically (user may have edited it after install).
rem
rem Exit: 0 ok, 1 nothing found (still ok), 2 arg error
rem NOTE: keep this file CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT="
set "CREO_HOME="
set "PURGE="
set "SILENT="
set "LOG="

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--root"      ( set "ROOT=%~2"      & shift & shift & goto :parse )
if /i "%~1"=="--creo-home" ( set "CREO_HOME=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--log"       ( set "LOG=%~2"       & shift & shift & goto :parse )
if /i "%~1"=="--purge"     ( set "PURGE=1" & shift & goto :parse )
if /i "%~1"=="--silent"    ( set "SILENT=1" & shift & goto :parse )
echo [ERR] unknown argument: %~1
exit /b 2
:parsed

if not defined ROOT (
    call :say "[ERR] --root is required"
    exit /b 2
)
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

set "FOUND=0"

rem ---------------- 1) config.pro marker ----------------
if not defined CREO_HOME goto :cfg_no_home
for %%I in ("%CREO_HOME%") do set "CREO_HOME=%%~fI"
set "CFG=%CREO_HOME%\config.pro"
if not exist "%CFG%" goto :cfg_missing
findstr /c:"creo-hub.dat" "%CFG%" >nul
if errorlevel 1 goto :cfg_clean
findstr /v /c:"# >>> creo-hub" /c:"# <<< creo-hub" /c:"creo-hub.dat" "%CFG%" > "%CFG%.creo-hub-tmp"
if not exist "%CFG%.creo-hub-tmp" goto :cfg_missing
move /y "%CFG%.creo-hub-tmp" "%CFG%" >nul
set "FOUND=1"
call :say "[1/2] marker block removed from %CFG%"
goto :regfile

:cfg_no_home
call :say "[1/2] --creo-home not given; config.pro untouched"
goto :regfile

:cfg_missing
call :say "[1/2] %CFG% not found (nothing to remove)"
goto :regfile

:cfg_clean
call :say "[1/2] no marker block in %CFG% (nothing to remove)"

:regfile

rem ---------------- 2) registry file ----------------
if exist "%ROOT%\creo-hub.dat" (
    del /q "%ROOT%\creo-hub.dat"
    set "FOUND=1"
    call :say "[2/2] removed %ROOT%\creo-hub.dat"
) else (
    call :say "[2/2] %ROOT%\creo-hub.dat not found (nothing to remove)"
)

if defined PURGE (
    if exist "%ROOT%" (
        rmdir /s /q "%ROOT%" 2>nul
        call :say "[purge] removed tree %ROOT% (plugins\logs inside are gone)"
    )
)

if "%FOUND%"=="0" (
    call :say "[done] nothing to uninstall (idempotent no-op)"
) else (
    call :say "[done] registration removed; payload kept (use --purge to delete files)"
)
call :say "[note] config.pro backup (*.creo-hub-bak) is kept for manual review"
exit /b 0

:say
echo %~1
if defined LOG echo %~1>>"%LOG%"
exit /b 0
