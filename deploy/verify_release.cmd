@echo off
rem =====================================================================
rem verify_release.cmd - verify dist\ artifacts (existence + SHA256 + paths).
rem Usage: deploy\verify_release.cmd [--dist <dir>]
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
where node >nul 2>&1
if errorlevel 1 ( echo [ERR] node not found & popd & exit /b 2 )
node deploy\verify_release.mjs %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
