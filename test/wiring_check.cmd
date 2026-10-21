@echo off
rem =====================================================================
rem creo-hub production wiring check (added 2026-09-13, R12).
rem Field bug: every offline suite registers its OWN httpd API handler,
rem so hub_main.c forgetting httpd_set_api_handler() went unnoticed -
rem all /api/v1/* answered an unlogged httpd-level 404 in production.
rem This script asserts the CRITICAL wiring lines exist in hub_main.c.
rem Usage: wiring_check.cmd        (exit 0 = all present, 1 = missing)
rem NOTE: CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
set "SRC=%ROOT%\src\hub\hub_main.c"
set /a FAILS=0
set /a N=0

if exist "%SRC%" goto :src_ok
echo [ERR] %SRC% not found
exit /b 1
:src_ok

call :chk "httpd_set_api_handler(web_api_route" "API router registered (A-5 wiring - the 9/13 field bug)"
call :chk "web_api_init()" "web_api_init called"
call :chk "web_api_set_token(hub_token_value" "session token wired to API"
call :chk "hub_token_write_file(g_root" "token file written (G4)"
call :chk "web_api_set_wakeup(hub_wakeup_post" "E6 wakeup hook wired"
call :chk "ProUITimerCreate(hub_timer_action" "500ms timer fallback created"
call :chk "httpd_start(HTTPD_PORT_FIRST" "httpd started on scan window"
call :chk "web_api_set_executor_ready(1)" "executor armed AFTER httpd up"
call :chk "web_api_stop_waiters()" "terminate phase 1b (R10 stall fix)"
call :chk "registry_unmount(i, 1)" "terminate unloads ACTIVE plugins (R11 E_IN_USE fix)"
call :chk "hub_token_delete_file(g_root)" "token file removed on terminate"
call :chk "state_save(g_root)" "state persisted on terminate"

echo.
if %FAILS%==0 goto :all_ok
echo === WIRING RESULT: %N% checked, %FAILS% MISSING - FAIL ===
exit /b 1
:all_ok
echo === WIRING RESULT: %N% checked, 0 missing - ALL PASS ===
exit /b 0

:chk
rem goto-label style on purpose: parens inside %2 (descriptions like
rem "(R10 stall fix)") would tear apart an if/else block - known cmd trap.
set /a N+=1
findstr /C:%1 "%SRC%" >nul
if errorlevel 1 goto :chk_fail
echo [PASS] %~2
goto :eof
:chk_fail
echo [FAIL] %~2
set /a FAILS+=1
goto :eof
