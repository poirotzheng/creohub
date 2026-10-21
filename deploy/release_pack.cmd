@echo off
rem =====================================================================
rem creo-hub release packer (M4c; docs\39 6.3) - NO Node required
rem
rem   release_pack.cmd [--ver 1.0.0] [--out dist] [--profile both^|win10^|win7]
rem
rem Produces:
rem   dist\creo-hub-<ver>-win10\   full profile (hub + panels + plugins +
rem                                installer scripts + bridge + agent + vendor)
rem   dist\creo-hub-<ver>-win7\    minimal profile (hub + panels + plugins +
rem                                installer scripts; NO node/dsh/bridge)
rem   dist\creo-hub-mcp-<ver>\     bridge package for any MCP host
rem   dist\SHA256SUMS.txt          sha256 of every produced file
rem   dist\creo-hub-<ver>-win10.zip / -win7.zip
rem
rem Payload is copied from THIS repo (tracked sources only; bin DLLs are the
rem built artifacts). Optional vendor\ dirs (node portable / dsh tree) are
rem copied when present.
rem Exit: 0 ok, 1 copy/zip failure, 2 args
rem NOTE: keep CRLF + pure ASCII.
rem =====================================================================
setlocal
set "VER=1.0.0"
set "OUT=dist"
set "PROFILE=both"
rem IMPORTANT: capture the script dir BEFORE the shift-based arg loop -
rem shift also shifts %0, so %~dp0 after parsing points at the CWD.
for %%I in ("%~dp0..") do set "ROOT=%%~fI"

:parse
if "%~1"=="" goto :parsed
if /i "%~1"=="--ver"     ( set "VER=%~2"     & shift & shift & goto :parse )
if /i "%~1"=="--out"     ( set "OUT=%~2"     & shift & shift & goto :parse )
if /i "%~1"=="--profile" ( set "PROFILE=%~2" & shift & shift & goto :parse )
echo [ERR] unknown argument: %~1
exit /b 2
:parsed

if not exist "%ROOT%\build\out\creo_hub.dll" (
    echo [ERR] payload missing: %ROOT%\build\out\creo_hub.dll - run build\build.cmd first
    exit /b 1
)
pushd "%ROOT%"

if /i "%PROFILE%"=="both"  goto :both
if /i "%PROFILE%"=="win10" goto :win10
if /i "%PROFILE%"=="win7"  goto :win7
echo [ERR] bad --profile %PROFILE%
popd
exit /b 2

:both
call :pack win10 || (popd & exit /b 1)
call :pack win7  || (popd & exit /b 1)
goto :mcp

:win10
call :pack win10 || (popd & exit /b 1)
goto :mcp

:win7
call :pack win7 || (popd & exit /b 1)
goto :mcp

:mcp
if /i "%PROFILE%"=="win7" goto :sums
call :packmcp || (popd & exit /b 1)

:sums
call :hashall
echo === RELEASE PACK OK: %OUT% (ver=%VER%, profile=%PROFILE%) ===
popd
exit /b 0

rem ---------------------------------------------------------------- pack <profile>
:pack
set "PF=%~1"
set "DST=%OUT%\creo-hub-%VER%-%PF%"
echo [pack %PF%] %DST%
if exist "%DST%" rmdir /s /q "%DST%"
mkdir "%DST%\build\out"     2>nul
mkdir "%DST%\web"           2>nul
mkdir "%DST%\hub_text"      2>nul
mkdir "%DST%\plugins"       2>nul
mkdir "%DST%\deploy"        2>nul
mkdir "%DST%\docs"          2>nul

copy /y "build\out\creo_hub.dll" "%DST%\build\out\creo_hub.dll" >nul || exit /b 1
robocopy "web" "%DST%\web" /e /njh /njs /ndl /nfl >nul
if errorlevel 8 exit /b 1
if exist "hub_text" robocopy "hub_text" "%DST%\hub_text" /e /njh /njs /ndl /nfl >nul
rem exclude deliberate test fixtures (bad1/demofail/demoslot) from user distributions
if exist "plugins"  robocopy "plugins"  "%DST%\plugins"  /e /njh /njs /ndl /nfl /xd bad1 demofail demoslot >nul
copy /y "deploy\install.cmd"   "%DST%\deploy\" >nul
copy /y "deploy\uninstall.cmd" "%DST%\deploy\" >nul
copy /y "deploy\doctor.cmd"    "%DST%\deploy\" >nul
copy /y "deploy\installer\setup.iss" "%DST%\deploy\" >nul 2>nul
copy /y "README.md" "%DST%\README.md" >nul 2>nul
if exist "docs\18_*.md" copy /y "docs\18_*.md" "%DST%\docs\" >nul
if exist "docs\39_*.md" copy /y "docs\39_*.md" "%DST%\docs\" >nul
if exist "docs\40_*.md" copy /y "docs\40_*.md" "%DST%\docs\" >nul

if /i "%PF%"=="win10" (
    if exist "bridge" robocopy "bridge" "%DST%\bridge" /e /njh /njs /ndl /nfl /xd "bridge\test" /xf "*.log" >nul
    if exist "agent"  robocopy "agent"  "%DST%\agent"  /e /njh /njs /ndl /nfl /xd test /xf *.log >nul
    if exist "vendor\node" robocopy "vendor\node" "%DST%\vendor\node" /e /njh /njs /ndl /nfl >nul
    if exist "vendor\dsh"  robocopy "vendor\dsh"  "%DST%\vendor\dsh"  /e /njh /njs /ndl /nfl >nul
)
exit /b 0

rem ---------------------------------------------------------------- mcp package
:packmcp
set "MCP=%OUT%\creo-hub-mcp-%VER%"
echo [pack mcp] %MCP%
if exist "%MCP%" rmdir /s /q "%MCP%"
mkdir "%MCP%" 2>nul
copy /y "bridge\bridge.mjs" "%MCP%\" >nul || exit /b 1
copy /y "bridge\package.json" "%MCP%\" >nul
copy /y "bridge\README.zh.md" "%MCP%\" >nul 2>nul
robocopy "bridge\lib" "%MCP%\lib" /e /njh /njs /ndl /nfl >nul
if errorlevel 8 exit /b 1
if exist "bridge\node_modules" robocopy "bridge\node_modules" "%MCP%\node_modules" /e /njh /njs /ndl /nfl >nul
if errorlevel 8 exit /b 1
exit /b 0

rem ---------------------------------------------------------------- sha256 + zip
rem Integrity strategy: hash the ZIPs (and the setup.exe appended by
rem build_installer.cmd) instead of every file - node_modules trees are
rem thousands of files and the zip hash already covers them. A per-package
rem MANIFEST.txt (path+size listing) is written for quick diffing.
:zipall
for %%D in (creo-hub-%VER%-win10 creo-hub-%VER%-win7) do (
    if exist "%OUT%\%%D" (
        if exist "%OUT%\%%D.zip" del "%OUT%\%%D.zip"
        powershell -NoProfile -Command "Compress-Archive -Path '%OUT%\%%D\*' -DestinationPath '%OUT%\%%D.zip' -Force" >nul
        dir /s /b "%OUT%\%%D" > "%OUT%\%%D.MANIFEST.txt"
    )
)
exit /b 0

:hashall
set "SUMS=%OUT%\SHA256SUMS.txt"
if exist "%SUMS%" del "%SUMS%"
call :zipall
for %%F in ("%OUT%\*.zip") do (
    for /f "usebackq delims=" %%H in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%%~fF').Hash.ToLower()"`) do >> "%SUMS%" echo %%H  %%~nxF
)
if exist "%OUT%\creo-hub-%VER%-setup.exe" (
    for /f "usebackq delims=" %%H in (`powershell -NoProfile -Command "(Get-FileHash -Algorithm SHA256 -LiteralPath '%OUT%\creo-hub-%VER%-setup.exe').Hash.ToLower()"`) do >> "%SUMS%" echo %%H  creo-hub-%VER%-setup.exe
)
exit /b 0
