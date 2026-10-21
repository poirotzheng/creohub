@echo off
rem =====================================================================
rem creo-hub plugin_pack gate (M4a A5, docs\39 5.3)
rem [1/5] generate fixture package (plugin_init, non-interactive)
rem [2/5] pack -> zip + packages.json index
rem [3/5] pack again -> deterministic sha256 (same bytes)
rem [4/5] pack_check: zip structure + index row + sha256 binding
rem [5/5] negative: package without plugin.json must be rejected
rem Exit: 0 = ALL PASS, 1 = FAIL, 2 = tooling/precondition error
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"
set "OUT=test\out"
if defined CREO_HUB_OUT set "OUT=%CREO_HUB_OUT%"

set "WORK=%OUT%\pack_check"
if exist "%WORK%" rmdir /s /q "%WORK%"
mkdir "%WORK%" 2>nul

echo [1/5] generate fixture package
node tools\plugin_init\plugin_init.mjs "%WORK%\pkg" --id packfix --name "Pack Fixture" --cmd "ping:safe;set_it:confirm:n:number" --requires demo --non-interactive --skip-lint
if errorlevel 1 (
    echo === PACK GATE: FIXTURE GENERATE FAIL ===
    popd
    exit /b 1
)

echo [2/5] pack (first run)
node tools\plugin_pack\plugin_pack.mjs "%WORK%\pkg" --out "%WORK%\dist" --json > "%WORK%\pack1.json"
if errorlevel 1 (
    echo === PACK GATE: PACK FAIL ===
    popd
    exit /b 1
)
if not exist "%WORK%\dist\packfix-0.1.0.zip" (
    echo === PACK GATE: ZIP MISSING ===
    popd
    exit /b 1
)

echo [3/5] pack again (determinism)
node tools\plugin_pack\plugin_pack.mjs "%WORK%\pkg" --out "%WORK%\dist" --json > "%WORK%\pack2.json"
if errorlevel 1 (
    echo === PACK GATE: PACK RUN2 FAIL ===
    popd
    exit /b 1
)
node -e "const a=require('fs').readFileSync(process.argv[1],'utf8'),b=require('fs').readFileSync(process.argv[2],'utf8');const ja=JSON.parse(a),jb=JSON.parse(b);process.exit(ja.sha256===jb.sha256?0:1)" "%WORK%\pack1.json" "%WORK%\pack2.json"
if errorlevel 1 (
    echo === PACK GATE: NON-DETERMINISTIC ZIP ===
    popd
    exit /b 1
)

echo [4/5] pack_check (zip + index)
node test\pack_check.mjs "%WORK%\dist\packfix-0.1.0.zip" "%WORK%\dist\packages.json" packfix 0.1.0
if errorlevel 1 (
    echo === PACK GATE: PACK CHECK FAIL ===
    popd
    exit /b 1
)

echo [5/5] negative: package without plugin.json
if not exist "%WORK%\badpkg" mkdir "%WORK%\badpkg"
node tools\plugin_pack\plugin_pack.mjs "%WORK%\badpkg" --out "%WORK%\dist" --no-lint >nul 2>nul
if not errorlevel 1 (
    echo === PACK GATE: BAD PACKAGE NOT REJECTED ===
    popd
    exit /b 1
)

echo === PACK GATE: ALL PASS ===
popd
exit /b 0
