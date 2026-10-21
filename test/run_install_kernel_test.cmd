@echo off
rem =====================================================================
rem creo-hub installer kernel gate (M4a A6 / T7; docs\39 6.1-6.2)
rem Offline fixture: fake payload + fake config.pro; no Creo, no node required
rem by the kernel itself (the GATE uses node only for assertions).
rem [1/6] doctor on empty root -> must FAIL
rem [2/6] install -> files + creo-hub.dat + config.pro marker + backup
rem [3/6] install again -> idempotent (exactly one marker block)
rem [4/6] doctor on installed root -> PASS
rem [5/6] uninstall -> marker removed, user line kept, payload kept
rem [6/6] negative: install with missing payload -> must FAIL
rem Exit: 0 = ALL PASS, 1 = FAIL, 2 = tooling error
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

set "WORK=%OUT%\install_k"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%\src\build\out" 2>nul
mkdir "%WORK%\src\web" 2>nul
mkdir "%WORK%\src\hub_text" 2>nul
mkdir "%WORK%\src\plugins\demo" 2>nul
mkdir "%WORK%\creohome" 2>nul
> "%WORK%\src\build\out\creo_hub.dll" echo fake-dll
> "%WORK%\src\web\index.html" echo ^<html^>^</html^>
> "%WORK%\src\hub_text\readme.txt" echo fake-text
> "%WORK%\src\plugins\demo\plugin.json" echo {}
> "%WORK%\creohome\config.pro" echo user-line-keep

echo [1/6] doctor on empty root (expect FAIL)
call deploy\doctor.cmd --root "%WORK%\missing_root" --creo-home "%WORK%\creohome" --mode core --silent --report "%WORK%\doctor1.txt"
if not errorlevel 1 (
    echo === INSTALL GATE: DOCTOR DID NOT FAIL ON EMPTY ROOT ===
    popd
    exit /b 1
)

echo [2/6] install (core mode)
call deploy\install.cmd --src "%WORK%\src" --root "%WORK%\root" --creo-home "%WORK%\creohome" --mode core --silent --yes
if errorlevel 1 (
    echo === INSTALL GATE: INSTALL FAIL ===
    popd
    exit /b 1
)
if not exist "%WORK%\root\build\out\creo_hub.dll" ( echo === INSTALL GATE: DLL NOT COPIED === & popd & exit /b 1 )
if not exist "%WORK%\root\creo-hub.dat" ( echo === INSTALL GATE: DAT MISSING === & popd & exit /b 1 )
if not exist "%WORK%\creohome\config.pro.creo-hub-bak" ( echo === INSTALL GATE: BACKUP MISSING === & popd & exit /b 1 )
node -e "const fs=require('fs');const d=fs.readFileSync(process.argv[1],'utf8');for(const k of ['name creo_hub','startup dll','exec_file','text_dir','allow_stop TRUE','revision Creo6.0','end'])if(d.indexOf(k)<0)process.exit(1)" "%WORK%\root\creo-hub.dat"
if errorlevel 1 (
    echo === INSTALL GATE: REGISTRY FILE INCOMPLETE ===
    popd
    exit /b 1
)
node -e "const fs=require('fs');const c=fs.readFileSync(process.argv[1],'utf8');if((c.match(/creo-hub\.dat/g)||[]).length!==1)process.exit(1);if(c.indexOf('PROTKDAT')<0)process.exit(1);if(c.indexOf('user-line-keep')<0)process.exit(1)" "%WORK%\creohome\config.pro"
if errorlevel 1 (
    echo === INSTALL GATE: CONFIG.PRO MARKER/BACKUP CONTENT FAIL ===
    popd
    exit /b 1
)

echo [3/6] install again (idempotent)
call deploy\install.cmd --src "%WORK%\src" --root "%WORK%\root" --creo-home "%WORK%\creohome" --mode core --silent --yes
if errorlevel 1 (
    echo === INSTALL GATE: REINSTALL FAIL ===
    popd
    exit /b 1
)
node -e "const fs=require('fs');const c=fs.readFileSync(process.argv[1],'utf8');if((c.match(/creo-hub\.dat/g)||[]).length!==1)process.exit(1);if(c.indexOf('PROTKDAT')<0)process.exit(1);if(c.indexOf('user-line-keep')<0)process.exit(1)" "%WORK%\creohome\config.pro"
if errorlevel 1 (
    echo === INSTALL GATE: REINSTALL PRODUCED DUPLICATE MARKERS ===
    popd
    exit /b 1
)

echo [4/6] doctor on installed root (expect PASS)
call deploy\doctor.cmd --root "%WORK%\root" --creo-home "%WORK%\creohome" --mode core --silent --report "%WORK%\doctor2.txt"
if errorlevel 1 (
    echo === INSTALL GATE: DOCTOR FAIL AFTER INSTALL ===
    popd
    exit /b 1
)

echo [5/6] uninstall
call deploy\uninstall.cmd --root "%WORK%\root" --creo-home "%WORK%\creohome" --silent
if errorlevel 1 (
    echo === INSTALL GATE: UNINSTALL FAIL ===
    popd
    exit /b 1
)
if exist "%WORK%\root\creo-hub.dat" ( echo === INSTALL GATE: DAT NOT REMOVED === & popd & exit /b 1 )
node -e "const fs=require('fs');const c=fs.readFileSync(process.argv[1],'utf8');if(c.indexOf('creo-hub.dat')>=0)process.exit(1);if(c.indexOf('user-line-keep')<0)process.exit(1)" "%WORK%\creohome\config.pro"
if errorlevel 1 (
    echo === INSTALL GATE: UNINSTALL LEFT MARKER OR ATE USER LINE ===
    popd
    exit /b 1
)
if not exist "%WORK%\root\build\out\creo_hub.dll" ( echo === INSTALL GATE: PAYLOAD SHOULD BE KEPT === & popd & exit /b 1 )

echo [6/6] negative: missing payload
call deploy\install.cmd --src "%WORK%\empty_src" --root "%WORK%\root2" --creo-home "%WORK%\creohome" --mode core --silent --yes >nul 2>nul
if not errorlevel 1 (
    echo === INSTALL GATE: MISSING PAYLOAD NOT REJECTED ===
    popd
    exit /b 1
)

echo === INSTALL GATE: ALL PASS ===
popd
exit /b 0
