@echo off
rem =====================================================================
rem creo-hub API probe helper (docs\17 C2/C7 companion).
rem Reads <root>\hub_token.txt (valid only while the hub runs in Creo),
rem probes the live API WITH the token header, then once WITHOUT (401).
rem Usage: probe_api.cmd [port]        (default port 8765)
rem Requires curl.exe (built into Win10 1803+ / Win11).
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
set "PORT=%~1"
if "%PORT%"=="" set "PORT=8765"
set "TOKFILE=%ROOT%\hub_token.txt"

if exist "%TOKFILE%" goto :tok_ok
echo [ERR] %TOKFILE% not found - is the hub running in Creo right now?
echo       (the token file only exists while the hub lives; it is deleted on stop)
echo.
pause
exit /b 1
:tok_ok
set /p TOKEN=<"%TOKFILE%"
echo token file read OK (32 hex chars expected; value NOT echoed here)

echo.
echo === [1/4] GET /api/v1/health  (with token) ===
curl.exe -s -H "X-Hub-Token: %TOKEN%" http://127.0.0.1:%PORT%/api/v1/health
echo.

echo === [2/4] GET /api/v1/plugins (with token) ===
curl.exe -s -H "X-Hub-Token: %TOKEN%" http://127.0.0.1:%PORT%/api/v1/plugins
echo.

echo === [3/4] GET /api/v1/log?offset=0 (with token, first bytes) ===
curl.exe -s -H "X-Hub-Token: %TOKEN%" "http://127.0.0.1:%PORT%/api/v1/log?offset=0"
echo.

echo === [4/4] GET /api/v1/health  WITHOUT token (expect HTTP/1.1 401) ===
curl.exe -s -i http://127.0.0.1:%PORT%/api/v1/health | findstr /C:"HTTP/1.1" /C:"unauthorized"
echo.
echo === probe done (4 sections above; paste into docs\17 C2 record) ===
echo.
pause
exit /b 0
