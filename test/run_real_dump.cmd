@echo off
rem =====================================================================
rem real-data panel render check (field-issue repro, 2026-09-13)
rem   usage: run_real_dump.cmd <port> <token> [browser]
rem   dumps http://127.0.0.1:port/?token=... via headless browser and
rem   greps the rendered DOM for panel markers vs error pages.
rem   NOTE: CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"
set "PORT=%~1"
set "TOKEN=%~2"
set "BROWSER=%~3"
if "%BROWSER%"=="" set "BROWSER=C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe"
if not exist "%BROWSER%" set "BROWSER=%ProgramFiles%\Google\Chrome\Application\chrome.exe"
if not exist "%BROWSER%" echo [ERR] no browser & popd & exit /b 3

set "DUMP=%OUT%\dump_real.html"
if exist "%DUMP%" del "%DUMP%"
"%BROWSER%" --headless=new --no-sandbox --disable-gpu --disable-crash-reporter --no-first-run --no-default-browser-check --user-data-dir="%TEMP%\creo_hub_edge_rd" --virtual-time-budget=12000 --dump-dom "http://127.0.0.1:%PORT%/?token=%TOKEN%" > "%DUMP%" 2> %OUT%\chrome_real_err.txt
echo browser exit: %ERRORLEVEL%

for %%F in ("%DUMP%") do echo dump bytes: %%~zF
findstr /C:"id=\"jserr\"" "%DUMP%" >nul && echo MARK jserr-bar: FIRED || echo MARK jserr-bar: absent
findstr /C:"Demo Plugin" "%DUMP%" >nul && echo MARK demo-card: PRESENT || echo MARK demo-card: MISSING
findstr /C:"stActive" "%DUMP%" >nul && echo MARK stats-rendered: YES || echo MARK stats-rendered: NO
findstr /C:"demo.hello" "%DUMP%" >nul && echo MARK cmd-id: PRESENT || echo MARK cmd-id: MISSING
popd
exit /b 0
