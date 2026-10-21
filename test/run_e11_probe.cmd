@echo off
rem =====================================================================
rem creo-hub M3a E11 probe wrapper (needs LIVE Creo + dev-hub session)
rem Passes all args through: run_e11_probe.cmd --case all-b  etc.
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
pushd "%~dp0.."
node test\e11_probe.mjs %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
