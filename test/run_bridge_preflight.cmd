@echo off
setlocal
rem ============================================================
rem  creo-hub M2.5 bridge PREFLIGHT runner (docs\21 s1 step 0)
rem  Double-click OK. Run BEFORE restarting dsh for field round.
rem  Read-only checks: node/bridge/SDK/user patch/hub probe/ask-gate.
rem ============================================================
cd /d "%~dp0.."

where node >nul 2>&1
if errorlevel 1 goto NONODE

node "test\bridge_preflight.mjs"
set RC=%errorlevel%
if "%RC%"=="0" goto READY

echo.
echo [preflight] NOT READY - fix every [FAIL] line above, then re-run
goto END

:READY
echo.
echo [preflight] READY - you may now restart dsh and start the field round
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
