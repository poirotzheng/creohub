@echo off
rem =====================================================================
rem P12 helper: deterministic demo-manifest swap (2026-09-13, R13;
rem R14: double-click friendly - no-arg mode prompts interactively).
rem Field round showed the manual Explorer copy never landed (plugin.json
rem mtime unchanged, no fingerprint CHANGED in log) - hidden extensions /
rem wrong paste target are classic traps. This script does exact copies
rem and self-verifies, so P12 becomes reproducible.
rem
rem usage: double-click (then type v2 / v1 / status at the prompt)
rem        p12_swap.cmd v2      install v2 upgrade (backs up original once)
rem        p12_swap.cmd v1      restore the original v1 manifest
rem        p12_swap.cmd status  show which manifest is currently installed
rem NOTE: CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
set "PJ=%ROOT%\plugins\demo\plugin.json"
set "BAK=%ROOT%\plugins\demo\plugin.json.p12bak"
set "V2=%ROOT%\test\p12_demo_plugin_v2.json"
set "MODE=%~1"
if not "%MODE%"=="" goto :dispatch

echo ============================================================
echo   P12 demo-manifest swap tool   (plugins\demo\plugin.json)
echo ============================================================
echo.
echo     v2      install the v2 UPGRADE manifest
echo             (original is backed up once to plugin.json.p12bak)
echo     v1      RESTORE the original v1 manifest from that backup
echo     status  show which manifest is installed right now
echo.
set /p MODE=Type v2 / v1 / status and press Enter: 

:dispatch
if /I "%MODE%"=="v2" goto :do_v2
if /I "%MODE%"=="v1" goto :do_v1
if /I "%MODE%"=="status" goto :do_status
echo.
echo [ERR] invalid mode "%MODE%"
echo usage: double-click and type v2/v1/status, or: p12_swap.cmd v2 ^| v1 ^| status
echo.
pause
exit /b 2

:do_v2
if not exist "%PJ%" echo [ERR] %PJ% missing & goto :end_fail
if not exist "%V2%" echo [ERR] %V2% missing & goto :end_fail
if not exist "%BAK%" copy /Y "%PJ%" "%BAK%" >nul
copy /Y "%V2%" "%PJ%" >nul
if errorlevel 1 echo [ERR] copy failed & goto :end_fail
echo [OK] v2 upgrade installed (original backed up to plugin.json.p12bak)
goto :verify

:do_v1
if not exist "%BAK%" echo [ERR] no backup found - nothing to restore & goto :end_fail
copy /Y "%BAK%" "%PJ%" >nul
if errorlevel 1 echo [ERR] restore failed & goto :end_fail
echo [OK] original v1 manifest restored
goto :verify

:do_status
echo current state of %PJ% :
goto :verify

:verify
echo.
for %%F in ("%PJ%") do echo   size=%%~zF bytes  mtime=%%~tF
findstr /C:"demo.export_summary" "%PJ%" >nul
if errorlevel 1 (echo   manifest = V1 original) else (echo   manifest = V2 upgrade)
echo.
echo Next: in the panel click "Refresh Registry", then watch for:
echo   panel : yellow badge on the demo card (manifest-changed)
echo   log   : [REG] manifest fingerprint CHANGED for demo ...
echo (badge clears only after Unmount -^> Mount; restoring v1 makes it
echo  reappear once - both are honest R2 semantics, see docs\17 P12)
echo.
pause
exit /b 0

:end_fail
echo.
pause
exit /b 1
