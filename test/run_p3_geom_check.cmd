@echo off
rem =====================================================================
rem creo-hub M3d d1: demo3.boom prism geometry static gate (Q-M3d-4)
rem   [1/2] invariants over P3_EDGES/P3_FACES (src\demo3\demo3_adapter.c):
rem         right-hand frame / coplanarity / directed contour closure /
rem         manifold edge usage / Euler / material half-space / bbox
rem   [2/2] selftest: historical ISSUE-L2 buggy row + orient flip + dir
rem         flip must ALL be caught (gate detection proof)
rem Exit code: 0 = PASS, 1 = FAIL, 2 = usage/node error
rem NOTE: this file MUST stay CRLF + pure ASCII.
rem =====================================================================
setlocal
set "ROOT=%~dp0.."
pushd "%ROOT%"

where node >nul 2>&1
if errorlevel 1 (
  echo [ERR] node not found on PATH
  popd
  exit /b 2
)

echo [1/2] geometry invariants
node test\p3_geom_check.mjs
if errorlevel 1 (
  echo === P3 GEOM GATE: FAIL ===
  popd
  exit /b 1
)

echo.
echo [2/2] selftest (buggy variants must be caught)
node test\p3_geom_check.mjs --selftest
if errorlevel 1 (
  echo === P3 GEOM SELFTEST: FAIL ===
  popd
  exit /b 1
)

echo.
echo === P3 GEOM GATE: ALL PASS ===
popd
exit /b 0
