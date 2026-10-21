# creo-hub MCP 桥（creo-hub-bridge）

标准 **stdio MCP 服务**：把 creo-hub 暴露给 DSH / Claude Desktop / Cursor /
VS Code 等任意 MCP host。桥无状态——每次调用重新发现 hub 端口并现读令牌，
hub 重启/换端口/轮换令牌都无需改配置。

桥不修改 hub DLL，且不监听任何端口。

## 快速开始

```bat
cd <REPO_ROOT>\bridge
npm install
cd ..
test\run_bridge_battery.cmd
```

唯一依赖：`@modelcontextprotocol/sdk`（`package-lock.json` 已钉版本）。
需要 Node.js 18+。

## 配置

| 环境变量 | 含义 |
|---|---|
| `CREO_HUB_ROOT` | creo-hub 根目录绝对路径（必须含 `hub_token.txt`） |

DSH 用户见 `integrations/dsh/`（profile patch 或本地 bundle）；Claude Desktop /
Cursor 复制 `integrations/claude/mcp.json` 或
`integrations/cursor/mcp.json`，把 `<REPO_ROOT>` 换成实际路径。

示例：

```json
{
  "mcpServers": {
    "creohub": {
      "command": "cmd",
      "args": ["/c", "node", "<REPO_ROOT>\\bridge\\bridge.mjs"],
      "env": { "CREO_HUB_ROOT": "<REPO_ROOT>" }
    }
  }
}
```

## 工具面

| 工具 | 类别 | 语义 |
|---|---|---|
| `creo_hub_status` | 只读 | 端口发现/版本/features/执行者/队列/令牌有效性 |
| `creo_hub_plugins` | 只读 | 实时状态卡（state/fingerprint/autoMount/lastErr） |
| `creo_hub_manifest` | 只读 | 全量清单（命令契约含 risk/descForLLM/inputSchema） |
| `creo_hub_log` | 只读 | 增量审计尾读（按字节 offset） |
| `creo_hub_mount` `{id}` | 变更 | 挂载插件（ACTIVE 时幂等） |
| `creo_hub_unmount` `{id}` | 变更 | 卸载；`TAINTED` 表示 DLL 驻留，须重启 Creo，勿反复重试 |
| `creo_hub_call` `{plugin, command, args, dryRun?}` | 变更/只读 | 执行声明命令；confirm 类需面板批准 |
| `creo_hub_job` `{jobId?}` | 只读 | 轮询 async 任务（queued/working/completed/failed/cancelled） |

每条插件命令同时以一级工具暴露（`<plugin>__<command>`），列表随
`tools/list_changed` 动态更新；缓存工具列表的 host 可能需要刷新。

## 错误码

| `err_code` | 含义 → 动作 |
|---|---|
| `HUB_UNREACHABLE` | 8765~8775 无 hub → 启动 Creo / 确认 hub 同机运行 |
| `TOKEN_MISSING` | `<root>\hub_token.txt` 缺失或非 hex → 核对 `CREO_HUB_ROOT` |
| `TOKEN_STALE` | hub 拒绝令牌 → root 指向了另一套安装 |
| `BUSY_TIMEOUT` | Creo 主线程繁忙 → 查审计/日志，稍后重试 |
| `HUB_NOT_READY` / `HUB_STOPPING` | Creo 初始化/退出中 → 等待后重试 |
| `CONTRACT_MISMATCH` | 非 JSON/404 → 端口被其它程序占用，或 hub 过旧 |

## 安全不变量

- 仅 stdio；零监听端口。
- 令牌按调用读取，永不进日志/错误/返回值。
- stdout 只输出 JSON-RPC 帧（诊断走 stderr）。
- 审计以 hub 侧为唯一账本；桥无状态。

## 离线/气隙部署

桥只依赖 Node.js 与随包 `node_modules`（一个 SDK）。无网设备：在 staging 机
`npm install` 后整目录打包（同 OS/架构），`command` 指向 `node.exe` 绝对路径即可，
运行期零网络。
