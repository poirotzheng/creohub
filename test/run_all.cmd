@echo off
rem =====================================================================
rem run_all.cmd - run the offline gate suite (no Creo required).
rem
rem Usage:
rem   test\run_all.cmd
rem
rem Environment:
rem   CREO_HUB_OUT   output directory for compiled objects/exes
rem                  (default: %TEMP%\creo-hub-test-out)
rem   CI=1           suppresses interactive pauses (set automatically here)
rem
rem Exit: 0 = all gates pass, 1 = at least one failure, 2 = setup error.
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"

if not defined CREO_HUB_OUT set "CREO_HUB_OUT=%TEMP%\creo-hub-test-out"
set "CI=1"

where node >nul 2>&1
if errorlevel 1 echo [WARN] node not found - node gates will fail

set "FAILED=0"
set "PASSED=0"

call :gate run_registry_test.cmd
call :gate run_reconcile_test.cmd
call :gate run_depgraph_test.cmd
call :gate run_marsh_test.cmd
call :gate run_nonce_test.cmd
call :gate run_version_test.cmd
call :gate run_log_test.cmd
call :gate run_sdk_test.cmd
call :gate run_httpd_test.cmd
call :gate run_v2_manifest_test.cmd
call :gate run_call_test.cmd
call :gate run_web_api_test.cmd
call :gate run_p12_check.cmd
call :gate run_p3_geom_check.cmd
call :gate run_tool_contract_test.cmd
call :gate run_hooks_gate_test.cmd
call :gate run_install_kernel_test.cmd
call :gate run_plugin_tpl_check.cmd
call :gate run_plugin_init_check.cmd
call :gate run_manifest_lint.cmd
call :gate run_pack_test.cmd
call :gate run_agent_test.cmd
call :gate run_bridge_battery.cmd
call :gate run_bridge_preflight.cmd
call :gate run_dev_face_test.cmd

echo.
echo ============================================================
echo  OFFLINE GATES: %PASSED% passed, %FAILED% failed
echo  (live gates are separate: e11_probe / panel_e2e / real_dump)
echo ============================================================
popd
if "%FAILED%"=="0" exit /b 0
exit /b 1

:gate
echo.
echo === %~1
call "%~dp0%~1" ci
if errorlevel 1 goto :gate_fail
set /a PASSED+=1
echo [PASS] %~1
exit /b 0

:gate_fail
set /a FAILED+=1
echo [FAIL] %~1
exit /b 0
