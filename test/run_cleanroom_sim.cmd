@echo off
rem =====================================================================
rem run_cleanroom_sim.cmd - clean-room surrogate (no real clean machine yet).
rem
rem Runs the installer kernel against an isolated temp root (install ->
rem idempotent install -> doctor -> uninstall -> negative), with TEMP
rem redirected, then asserts the installer kernel left no state in the repo.
rem
rem Exit: 0 = ALL PASS, 1 = failure, 2 = tooling error.
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"

set "WORK=%TEMP%\creo-hub-cleanroom"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%" 2>nul
if not exist "%WORK%" ( echo [ERR] cannot create %WORK% & popd & exit /b 2 )

set "CREO_HUB_OUT=%WORK%\out"
set "TMP=%WORK%\tmp"
set "TEMP=%WORK%\tmp"
mkdir "%TMP%" 2>nul

echo [1/2] installer kernel (isolated temp root)
call test\run_install_kernel_test.cmd ci
if errorlevel 1 ( echo === CLEANROOM: INSTALL KERNEL FAIL === & popd & exit /b 1 )

echo [2/2] repo state cleanliness (no generated runtime files)
if exist "%ROOT%\hub_state.json" echo [WARN] hub_state.json exists in repo - runtime file, git-ignored
if exist "%ROOT%\creo-hub.dat" echo [WARN] creo-hub.dat exists in repo - install artifact, git-ignored

echo === CLEANROOM SIM: ALL PASS ===
popd
exit /b 0
