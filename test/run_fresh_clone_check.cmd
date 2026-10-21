@echo off
rem =====================================================================
rem run_fresh_clone_check.cmd - prove that a clean checkout builds and passes
rem the core offline gates (no reliance on untracked workspace files).
rem
rem Steps: git archive HEAD -> %TEMP%\creo-hub-fresh -> copy local.env ->
rem        build -> registry/manifest-lint/agent gates.
rem
rem Exit: 0 = ALL PASS, 1 = failure, 2 = tooling error.
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"

set "WORK=%TEMP%\creo-hub-fresh"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%" 2>nul
if not exist "%WORK%" ( echo [ERR] cannot create %WORK% & popd & exit /b 2 )

echo [1/5] git archive HEAD
git archive --format=zip --output="%WORK%\src.zip" HEAD
if errorlevel 1 ( echo === FRESH CLONE: ARCHIVE FAIL === & popd & exit /b 2 )

echo [2/5] extract to %WORK%
powershell -NoProfile -Command "Expand-Archive -LiteralPath '%WORK%\src.zip' -DestinationPath '%WORK%' -Force"
if errorlevel 1 ( echo === FRESH CLONE: EXTRACT FAIL === & popd & exit /b 2 )
del "%WORK%\src.zip" >nul 2>&1

if exist "%ROOT%\local.env" copy /y "%ROOT%\local.env" "%WORK%\local.env" >nul

pushd "%WORK%"
set "CREO_HUB_OUT=%WORK%\test-out"
set "CI=1"

echo [3/5] build
call build\build.cmd
if errorlevel 1 ( echo === FRESH CLONE: BUILD FAIL === & popd & popd & exit /b 1 )

echo [4/5] registry gate
call test\run_registry_test.cmd ci
if errorlevel 1 ( echo === FRESH CLONE: REGISTRY FAIL === & popd & popd & exit /b 1 )

echo [5/5] manifest lint + agent gate
call test\run_manifest_lint.cmd ci
if errorlevel 1 ( echo === FRESH CLONE: LINT FAIL === & popd & popd & exit /b 1 )
call test\run_agent_test.cmd ci
if errorlevel 1 ( echo === FRESH CLONE: AGENT FAIL === & popd & popd & exit /b 1 )

echo === FRESH CLONE: ALL PASS ===
popd
popd
exit /b 0
