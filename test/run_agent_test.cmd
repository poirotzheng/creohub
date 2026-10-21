@echo off
rem creo-hub hub-agent offline gate (M4d; docs39 7) - mock hub + fake LLM
rem Exit: 0 ALL PASS, 1 FAIL
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
echo [1/2] node agent\test\agent_test.mjs
node agent\test\agent_test.mjs
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" ( echo === AGENT GATE: FAIL & popd & exit /b 1 )
echo === AGENT GATE: ALL PASS ===
popd
exit /b 0