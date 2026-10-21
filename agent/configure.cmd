@echo off
rem =====================================================================
rem creo-hub agent configurator (M4d) - double-click, no deps, ASCII only.
rem Writes agent.config.json for the model endpoint of your choice.
rem Presets (OpenAI-compatible):
rem   1) vLLM / SGLang local or intranet   http://<host>:8000/v1
rem   2) Ollama                            http://127.0.0.1:11434/v1
rem   3) LM Studio                         http://127.0.0.1:1234/v1
rem   4) intranet OpenAI gateway (any path)
rem   5) DeepSeek public API               https://api.deepseek.com/v1
rem   6) custom
rem toolMode: native = function calling (recommended); json = fallback for
rem servers/models without tools support.
rem NOTE: keep CRLF + pure ASCII.
rem =====================================================================
setlocal EnableDelayedExpansion
set "HERE=%~dp0"

echo.
echo === creo-hub agent configuration ===
echo.
echo  1. vLLM / SGLang (OpenAI compatible, usually has tools support)
echo  2. Ollama (localhost:11434)
echo  3. LM Studio (localhost:1234)
echo  4. Intranet OpenAI-compatible gateway
echo  5. DeepSeek public API (needs key)
echo  6. Custom
set /p "PICK=Choose 1-6 [1]: "
if "%PICK%"=="" set "PICK=1"

if "%PICK%"=="1" (
    set "DEF_URL=http://127.0.0.1:11434/v1"
    set "DEF_MODEL=deepseek-v4-flash"
    set "DEF_TOOL=native"
)
if "%PICK%"=="2" (
    set "DEF_URL=http://127.0.0.1:11434/v1"
    set "DEF_MODEL=qwen2.5:7b"
    set "DEF_TOOL=json"
)
if "%PICK%"=="3" (
    set "DEF_URL=http://127.0.0.1:1234/v1"
    set "DEF_MODEL=local-model"
    set "DEF_TOOL=native"
)
if "%PICK%"=="4" (
    set "DEF_URL=http://127.0.0.1:11434/v1"
    set "DEF_MODEL=deepseek-v4-flash"
    set "DEF_TOOL=native"
)
if "%PICK%"=="5" (
    set "DEF_URL=https://api.deepseek.com/v1"
    set "DEF_MODEL=deepseek-chat"
    set "DEF_TOOL=native"
)
if "%PICK%"=="6" (
    set "DEF_URL=http://127.0.0.1:11434/v1"
    set "DEF_MODEL=model-name"
    set "DEF_TOOL=native"
)

echo.
set /p "BASEURL=baseURL [%DEF_URL%]: "
if "!BASEURL!"=="" set "BASEURL=%DEF_URL%"
set /p "MODEL=model name [%DEF_MODEL%]: "
if "!MODEL!"=="" set "MODEL=%DEF_MODEL%"
set /p "APIKEY=apiKey (empty if none): "
set /p "TOOLMODE=toolMode native^|json [%DEF_TOOL%]: "
if "!TOOLMODE!"=="" set "TOOLMODE=%DEF_TOOL%"
set /p "MAXR= maxRounds [6]: "
if "!MAXR!"=="" set "MAXR=6"

(
    echo {
    echo   "baseURL": "!BASEURL!",
    echo   "apiKey": "!APIKEY!",
    echo   "model": "!MODEL!",
    echo   "temperature": 0.2,
    echo   "maxRounds": !MAXR!,
    echo   "maxTokens": 1024,
    echo   "jobPollMs": 60000,
    echo   "toolMode": "!TOOLMODE!"
    echo }
) > "%HERE%agent.config.json"

echo.
echo [agent] wrote %HERE%agent.config.json
echo [agent] next: double-click start_agent.cmd, then open the panel - Assistant tab
pause
exit /b 0
