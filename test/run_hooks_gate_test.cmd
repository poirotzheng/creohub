@echo off
setlocal
rem ============================================================
rem  creo-hub ASK-GATE offline test runner (Q3 route A)
rem  Double-click OK. Verifies deploy\hooks.json + ask_gate.mjs
rem  + dsh_hooks_patch.yml against the REAL dsh-hook-protocol.
rem  Fully offline: no dsh / no Creo / no hub needed.
rem ============================================================
cd /d "%~dp0.."

where node >nul 2>&1
if errorlevel 1 goto NONODE

node "test\hooks_gate_test.mjs"
set RC=%errorlevel%
if "%RC%"=="0" goto READY

echo.
echo [ask-gate] NOT READY - fix every [FAIL] line above, then re-run
goto END

:READY
echo.
echo [ask-gate] READY - apply the patch snippet, restart dsh, retest mount
goto END

:NONODE
echo [ERR] node not found in PATH - install Node 18+ or fix PATH
set RC=2
goto END

:END
echo.
echo exit code: %RC%
if "%1"=="" if not defined CI pause
exit /b %RC%
