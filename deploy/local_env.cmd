@echo off
rem =====================================================================
rem local_env.cmd - resolve the PTC Pro/TOOLKIT SDK root into PTK.
rem
rem Called by build/test scripts. Resolution order:
rem   1) PTK (already set by the caller)
rem   2) CREO_TOOLKIT_ROOT environment variable
rem   3) PTK_DIR environment variable (legacy alias)
rem   4) <repo>\local.env  (CREO_TOOLKIT_ROOT=...)
rem   5) %ProgramFiles%\PTC\Creo*\Common Files\protoolkit
rem   6) %ProgramFiles(x86)%\PTC\Creo*\Common Files\protoolkit
rem   7) C:..F:\Program Files\PTC\Creo*\Common Files\protoolkit
rem
rem Exit: 0 = PTK set and verified, 2 = not found.
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."

if defined PTK goto :verify
if defined CREO_TOOLKIT_ROOT set "PTK=%CREO_TOOLKIT_ROOT%" & goto :verify
if defined PTK_DIR set "PTK=%PTK_DIR%" & goto :verify
if exist "%ROOT%\local.env" goto :from_file
goto :probe

:from_file
for /f "usebackq eol=# tokens=1,* delims==" %%a in ("%ROOT%\local.env") do if /i "%%a"=="CREO_TOOLKIT_ROOT" set "PTK=%%b"
if defined PTK goto :verify

:probe
for /d %%i in ("%ProgramFiles%\PTC\Creo*") do if exist "%%i\Common Files\protoolkit\includes\ProToolkit.h" set "PTK=%%i\Common Files\protoolkit"
if defined PTK goto :verify
for /d %%i in ("%ProgramFiles(x86)%\PTC\Creo*") do if exist "%%i\Common Files\protoolkit\includes\ProToolkit.h" set "PTK=%%i\Common Files\protoolkit"
if defined PTK goto :verify
for %%d in (C D E F G) do if exist "%%d:\Program Files\PTC\Creo*" for /d %%i in ("%%d:\Program Files\PTC\Creo*") do if exist "%%i\Common Files\protoolkit\includes\ProToolkit.h" set "PTK=%%i\Common Files\protoolkit"
if defined PTK goto :verify

:missing
echo [ERR] PTC Pro/TOOLKIT SDK not found.
echo        Set CREO_TOOLKIT_ROOT, or run: deploy\configure.cmd
echo        You need Creo Parametric 6.0 + the TOOLKIT license option.
endlocal
exit /b 2

:verify
if not exist "%PTK%\includes\ProToolkit.h" goto :missing
endlocal & set "PTK=%PTK%"
exit /b 0
