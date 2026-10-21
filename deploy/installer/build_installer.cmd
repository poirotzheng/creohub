@echo off
rem =====================================================================
rem creo-hub Inno Setup build (M4c; docs\39 6.1/6.6) - BUILD MACHINE ONLY
rem
rem   build_installer.cmd [--ver 1.0.0] [--profile win10^|win7] [--no-pack]
rem
rem Requires Inno Setup 6 (ISCC.exe). Produces dist\creo-hub-<ver>-setup.exe
rem Exit: 0 ok, 1 ISCC compile fail, 3 ISCC missing (tooling)
rem NOTE: keep CRLF + pure ASCII.
rem =====================================================================
setlocal
set "VER=1.0.0"
set "PROFILE=win10"
set "NOPACK="
rem IMPORTANT: capture the script dir BEFORE the shift-based arg loop -
rem shift also shifts %0, so %~dp0 after parsing points at the CWD.
for %%I in ("%~dp0..\..") do set "ROOT=%%~fI"

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--ver"     ( set "VER=%~2"     & shift & shift & goto :parse )
if /i "%~1"=="--profile" ( set "PROFILE=%~2" & shift & shift & goto :parse )
if /i "%~1"=="--no-pack" ( set "NOPACK=1"    & shift & goto :parse )
echo [ERR] unknown argument: %~1
exit /b 1
:parsed

pushd "%ROOT%"
if not exist "%ROOT%\build\out\creo_hub.dll" (
    echo [ERR] ROOT resolution failed: %ROOT% - run build\build.cmd first
    popd
    exit /b 1
)

if defined NOPACK goto :have_payload
if exist "dist\creo-hub-%VER%-%PROFILE%" goto :have_payload
call deploy\release_pack.cmd --ver %VER% --profile %PROFILE%
if errorlevel 1 ( echo [ERR] release_pack failed & popd & exit /b 1 )
:have_payload

set "ISCC="
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if defined ISCC goto :have_iscc
echo [ERR] ISCC.exe not found - install Inno Setup 6 build-time only; see docs39 6.6
popd
exit /b 3
:have_iscc

echo [build] ver=%VER% profile=%PROFILE%
for %%I in ("%ROOT%\dist\creo-hub-%VER%-%PROFILE%") do set "DISTABS=%%~fI"
for %%I in ("%ROOT%\dist") do set "OUTABS=%%~fI"
"%ISCC%" /Qp /DAppVer=%VER% "/DDistDir=%DISTABS%" "/DOutDir=%OUTABS%" "%ROOT%\deploy\installer\setup.iss"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo [ERR] ISCC failed rc=%RC%
    popd
    exit /b 1
)
if not exist "%ROOT%\dist\creo-hub-%VER%-setup.exe" (
    echo [ERR] setup.exe not produced
    popd
    exit /b 1
)
echo === INSTALLER BUILT: dist\creo-hub-%VER%-setup.exe ===
call :append_hash "%VER%"
popd
exit /b 0

rem ---------------------------------------------------------------- append hash
:append_hash
set "SUMS=dist\SHA256SUMS.txt"
set "EXE=dist\creo-hub-%~1-setup.exe"
if not exist "%SUMS%" goto :append_new
findstr /v /c:"creo-hub-%~1-setup.exe" "%SUMS%" > "%SUMS%.tmp"
if exist "%SUMS%.tmp" move /y "%SUMS%.tmp" "%SUMS%" >nul
:append_new
for /f "usebackq delims=" %%H in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%EXE%').Hash.ToLower()"`) do >> "%SUMS%" echo %%H  creo-hub-%~1-setup.exe
exit /b 0
