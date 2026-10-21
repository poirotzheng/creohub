@echo off
rem =====================================================================
rem creo-hub M3b b4: manifest_lint gate runner (DoD D5 node side)
rem   [1/3] regenerate shared fixtures + golden (single source of truth)
rem   [2/3] lint vs golden: both faces pinned to the same expectations
rem         (the C face runs inside run_v2_manifest_test.cmd, D5 phase)
rem   [3/3] informational lint over the REAL plugins tree (not a gate:
rem         bad1 is a deliberately INVALID fixture package)
rem Exit code: 0 = PASS, 1 = golden mismatch, 2 = usage/node error
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

echo [1/3] regenerate fixtures + golden
node test\make_lint_fixtures.mjs
if errorlevel 1 ( popd & exit /b 2 )

echo.
echo [2/3] lint vs golden (D5 contract, node face)
node test\manifest_lint.mjs test\lint_fixtures --golden test\lint_expected.txt
if errorlevel 1 (
  echo === MANIFEST LINT: GOLDEN MISMATCH ===
  popd
  exit /b 1
)

echo.
echo [3/3] informational: real plugins tree
node test\manifest_lint.mjs plugins
echo (informational only - bad1 is an intentional INVALID fixture)

echo.
echo === MANIFEST LINT: ALL PASS (node face; C face = run_v2_manifest_test D5) ===
popd
exit /b 0
