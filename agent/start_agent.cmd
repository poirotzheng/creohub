@echo off
rem =====================================================================
rem creo-hub agent launcher (M4d) - double-click friendly, no deps
rem Starts the local agent (loopback only) for the panel "助手" tab.
rem Node lookup: bundled vendor\node\node.exe first, else PATH node.
rem NOTE: keep CRLF + pure ASCII.
rem =====================================================================
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%.."

if not exist "%HERE%agent.config.json" (
    echo [agent] agent.config.json not found.
    echo         Run configure.cmd first to set your model endpoint.
    echo         Then start this file again.
    pause
    exit /b 1
)

set "NODE="
if exist "%ROOT%\vendor\node\node.exe" set "NODE=%ROOT%\vendor\node\node.exe"
if not defined NODE for %%N in (node.exe) do if not "%%~$PATH:N"=="" set "NODE=%%~$PATH:N"
if not defined NODE (
    echo [agent] node.exe not found. Install Node 18+ or ship vendor\node\node.exe.
    pause
    exit /b 1
)

echo [agent] starting with %NODE%
"%NODE%" "%HERE%hub-agent.mjs" %*
echo.
echo [agent] stopped. Press any key to close.
pause >nul
