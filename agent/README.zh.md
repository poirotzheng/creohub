# agent —— creo-hub 对话助手（M4d，形态 C）

零依赖 Node 服务（只监听 `127.0.0.1:8776~8785`），把 **OpenAI 兼容模型**（内网 DeepSeek V4 flash / vLLM / SGLang / Ollama / LM Studio / 公网 API）接到 hub 的命令工具面。

## 三步用起来

1. 双击 `configure.cmd` → 选预设（1 vLLM/SGLang、2 Ollama、3 LM Studio、4 内网网关、5 DeepSeek 公网、6 自定义）→ 填地址/模型名/key → 生成 `agent.config.json`；
2. 双击 `start_agent.cmd`（自动找 `vendor\node\node.exe` 或 PATH 里的 node）；
3. 打开面板 →「助手」tab → 对话。

## 工具面（固定 5 个）

`creo_hub_status / creo_hub_plugins / creo_hub_manifest / creo_hub_call / creo_hub_job`

## 两种 toolMode（模型兼容性）

| 模式 | 适用 | 说明 |
|---|---|---|
| `native`（默认） | vLLM/SGLang（DeepSeek 模板）、LM Studio、DeepSeek API | 用 OpenAI function calling（`tools`） |
| `json` | 不支持 tools 的服务/模型（部分 Ollama、老网关） | 模型每轮只输出 `{"tool":"…","args":{…}}` 或 `{"final":"…"}`，agent 解析执行 |

> 选择困难时：先试 `native`，若报 400 / 模型从不返回 tool_calls，就改 `json`。

## 安全与诚实边界

- 请求需 `X-Hub-Token`（=hub_token.txt）；CORS 仅允许本机环回面板 origin；
- **428 的 nonce 只在 agent 内存**，绝不进模型上下文；面板批准后同参数自动重试；
- async 命令（202+jobId）自动轮询 `creo_hub_job` 至终态再汇报；变更类命令不盲重试；
- key 只存 `agent.config.json`（已 gitignore）；`agent.log` 不落令牌/key/完整 args；**hub DLL 零外联不变**。

## 自测

```
test\run_agent_test.cmd
```
mock hub + fake LLM：**14 断言**（health/模型配置/401×2/native 工具循环/ACAO/403/400 + json 模式三连）。
