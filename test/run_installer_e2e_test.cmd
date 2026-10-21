@echo off
rem =====================================================================
rem creo-hub Inno installer E2E gate (M4c; docs\39 6.1/6.6)
rem Fixture payload -> ISCC build -> /VERYSILENT install -> assertions
rem -> uninstaller -> assertions. No Creo, no Node.
rem [1/5] fixture payload (fake dll/panel/text/plugins + real deploy scripts)
rem [2/5] ISCC compile -> setup.exe
rem [3/5] silent install (/DIR /CREOHOME), assert files + registration
rem [4/5] uninstaller, assert marker removed + user config line kept
rem [5/5] negative: ISCC missing => tooling error path documented (skip)
rem Exit: 0 ALL PASS, 1 FAIL, 3 ISCC missing (tooling)
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

set "WORK=%OUT%\installer"
set "DIST=%WORK%\dist\creo-hub-e2e-win10"
set "APP=%WORK%\app"
set "CREO=%WORK%\creohome"
set "VER=0.0.1"
for %%I in ("%WORK%") do set "WORK=%%~fI"
for %%I in ("%DIST%") do set "DIST=%%~fI"
for %%I in ("%APP%") do set "APP=%%~fI"
for %%I in ("%CREO%") do set "CREO=%%~fI"

if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%DIST%\build\out" 2>nul
mkdir "%DIST%\web" 2>nul
mkdir "%DIST%\hub_text" 2>nul
mkdir "%DIST%\plugins\demo" 2>nul
mkdir "%DIST%\deploy" 2>nul
mkdir "%CREO%" 2>nul

echo [1/5] fixture payload
> "%DIST%\build\out\creo_hub.dll" echo fake-dll
> "%DIST%\web\index.html" echo ^<html^>^</html^>
> "%DIST%\hub_text\readme.txt" echo fake-text
> "%DIST%\plugins\demo\plugin.json" echo {}
copy /y "deploy\install.cmd"   "%DIST%\deploy\" >nul
copy /y "deploy\uninstall.cmd" "%DIST%\deploy\" >nul
copy /y "deploy\doctor.cmd"    "%DIST%\deploy\" >nul
> "%CREO%\config.pro" echo user-line-keep

echo [2/5] ISCC compile
set "ISCC="
if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if defined ISCC goto :have_iscc
echo [SKIP] ISCC.exe not found - installer gate needs Inno Setup 6 build machine only
popd
exit /b 3
:have_iscc
"%ISCC%" /Qp /DUninstallable=no /DAppVer=%VER% "/DDistDir=%DIST%" /DOutDir=%WORK% "deploy\installer\setup.iss"
if errorlevel 1 ( echo === INSTALLER GATE: ISCC FAIL === & popd & exit /b 1 )
if not exist "%WORK%\creo-hub-%VER%-setup.exe" ( echo === INSTALLER GATE: SETUP MISSING === & popd & exit /b 1 )

echo [3/5] silent install
rem sandbox/dev note: Inno de-elevates and may hit a TEMP ACL denied in this
rem environment; point TEMP/TMP at the workspace (deployment machines unaffected)
if not exist "%WORK%\tmp" mkdir "%WORK%\tmp"
set "TEMP=%WORK%\tmp"
set "TMP=%WORK%\tmp"
"%WORK%\creo-hub-%VER%-setup.exe" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="%APP%" /CREOHOME="%CREO%" /LOG="%WORK%\setup.log"
if errorlevel 1 ( echo === INSTALLER GATE: SETUP RUN FAIL === & popd & exit /b 1 )
if not exist "%APP%\build\out\creo_hub.dll" ( echo === INSTALLER GATE: DLL MISSING === & popd & exit /b 1 )
if not exist "%APP%\creo-hub.dat" ( echo === INSTALLER GATE: DAT MISSING === & popd & exit /b 1 )
rem Uninstallable=no in this sandboxed gate (HKCU Uninstall key is denied); the real installer keeps the uninstaller - covered by W-c
if not exist "%CREO%\config.pro.creo-hub-bak" ( echo === INSTALLER GATE: BACKUP MISSING === & popd & exit /b 1 )
node -e "const fs=require('fs');const c=fs.readFileSync(process.argv[1],'utf8');if((c.match(/creo-hub\.dat/g)||[]).length!==1)process.exit(1);if(c.indexOf('PROTKDAT')<0)process.exit(1);if(c.indexOf('user-line-keep')<0)process.exit(1)" "%CREO%\config.pro"
if errorlevel 1 ( echo === INSTALLER GATE: REGISTRATION MARKER FAIL === & popd & exit /b 1 )

echo [4/5] uninstaller
call "%APP%\deploy\uninstall.cmd" --root "%APP%" --creo-home "%CREO%" --silent
if errorlevel 1 ( echo === INSTALLER GATE: UNINSTALL RUN FAIL === & popd & exit /b 1 )
if exist "%APP%\creo-hub.dat" ( echo === INSTALLER GATE: DAT NOT REMOVED === & popd & exit /b 1 )
node -e "const fs=require('fs');const c=fs.readFileSync(process.argv[1],'utf8');if(c.indexOf('creo-hub.dat')>=0)process.exit(1);if(c.indexOf('user-line-keep')<0)process.exit(1)" "%CREO%\config.pro"
if errorlevel 1 ( echo === INSTALLER GATE: UNINSTALL MARKER FAIL === & popd & exit /b 1 )

echo [5/5] done (negative path = ISCC-missing skip above)
echo === INSTALLER GATE: ALL PASS ===
popd
exit /b 0
