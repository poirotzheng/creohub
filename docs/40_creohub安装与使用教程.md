# 40 creo-hub 安装与使用教程（v1.0，面向最终用户/IT）

> **一句话**：装一次 → 打开 Creo（hub 自动运行）→ 浏览器面板管理插件；需要 AI 对话时再启动「助手」。
> 本文只讲**怎么做**；插件开发/契约细节见 `docs\18_插件开发者指南.md`。全程**离线可用、无需管理员、hub 不联网**。

---

## 0. 30 秒速览

| 步 | 做什么 | 成功标志 |
|---|---|---|
| 1 | 运行 `creo-hub-1.0.0-setup.exe`（或解压 zip 后双击 `deploy\install.cmd`） | 安装完成提示；目标目录出现 `build\out\creo_hub.dll`、`web\`、`creo-hub.dat` |
| 2 | 正常启动 Creo（无需任何手动操作） | Creo 菜单出现 `Utilities ▸ creo-hub`（或辅助应用程序列表里 `creo_hub` 状态=正在运行） |
| 3 | 点菜单 `Utilities ▸ creo-hub ▸ Open Web Panel` | 浏览器打开面板，顶部显示"运行 N / 共 M" |

> 之后所有插件都通过**面板**或**助手**管理，**不需要**再为每个插件改任何注册文件。

---

## 1. 系统要求

| 项 | 要求 |
|---|---|
| 操作系统 | Windows 7 SP1 x64 或 Windows 10/11 x64（安装器 `MinVersion=6.1sp1`） |
| Creo | Creo Parametric 6.0（Pro/TOOLKIT 6.0） |
| 权限 | 普通用户即可（安装器 `PrivilegesRequired=lowest`，装到当前用户目录） |
| 网络 | **不需要**（全离线；hub 只监听 `127.0.0.1`） |
| 运行库 | 无额外依赖（DLL 为 /MT 静态 CRT，Win7 自带组件即可） |
| Node（仅对话助手） | Node 18+；或使用 win10 安装包内附带的便携 Node |

---

## 2. 安装（两条路，选一条）

### 路线 A：安装包（推荐）

1. 双击 `creo-hub-1.0.0-setup.exe`；
2. 安装位置默认 `当前用户\Programs\creo-hub`（可改）；
3. 若安装时询问 **Creo 配置目录**（见 §7 如何找），填入；不确定可直接下一步（安装器会提示手工注册方法）；
4. 完成后按 §3 启动 Creo。

静默安装（IT 批量部署）：
```
creo-hub-1.0.0-setup.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="D:\apps\creo-hub" /CREOHOME="C:\Users\Public\Documents"
```

### 路线 B：压缩包（绿色）

1. 把 `creo-hub-1.0.0-win10.zip`（或 win7.zip）解压到任意目录，如 `D:\apps\creo-hub`；
2. 双击该目录下 `deploy\install.cmd`（或命令行加参数）：
```
deploy\install.cmd --root "D:\apps\creo-hub" --creo-home "C:\Users\Public\Documents"
```
3. 卸载：`deploy\uninstall.cmd --root "D:\apps\creo-hub" --creo-home "..."`（加 `--purge` 连文件一起删）。

### 安装器实际做了什么（5 件事）

1. 复制文件（`build\out`、`web`、`hub_text`、`plugins`、`deploy`，win10 档另含 `bridge`/`agent`）；
2. 生成注册文件 `<安装目录>\creo-hub.dat`；
3. 在 Creo 配置目录的 `config.pro` 里追加**3 行标记块**（首次写前备份为 `config.pro.creo-hub-bak`）：
```
# >>> creo-hub (managed by install.cmd; do not edit) >>>
PROTKDAT <安装目录>\creo-hub.dat
# <<< creo-hub <<<
```
4. 可选：创建"creo-hub 助手"快捷方式（开始菜单+桌面）与登录自启（安装时勾选，默认不勾；无 agent 的 win7 档自动跳过）；
5. 自检（文件/注册行/备份）并打印下一步提示。

> **卸载只会移除这 3 行和 `creo-hub.dat`**，不会覆盖你之后手动改过的 `config.pro`。

---

## 3. 首次启动与打开面板

1. **正常启动 Creo**——hub 会随 Creo 自动运行（`startup dll` 且未设 `delay_start`；真机已验证自动启动），**不需要**每次去辅助应用程序里点启动。
2. 打开面板，两种方式：
   - Creo 菜单：`Utilities ▸ creo-hub ▸ Open Web Panel`（推荐）；
   - 或浏览器打开：`http://127.0.0.1:8765/?token=<令牌>`，令牌在 `<安装目录>\hub_token.txt`（每次启动轮换）。
3. 面板四个区域：
   - **插件卡片**：每包状态（运行中/未挂载/错误）+ 挂载/卸载/版本切换；
   - **任务条**：长任务进度（5%→100%，带命令名与耗时）；
   - **确认抽屉**：confirm/destructive 命令的人工批准（60 秒内有效）；
   - **日志**：审计与诊断（每次操作都有 `[AUDIT]` 行）。

---

## 4. 用面板管理插件（日常最常用）

### 4.1 放一个插件包
把插件目录放到 **`<安装目录>\plugins\<插件id>\`**，结构：
```
plugins\my-plugin\
├── plugin.json          ← 清单（id/adapter/entry/commands 契约）
├── bin\my-plugin.dll    ← PTK 插件 DLL
└── text\text\usascii\   ← 消息资源（Creo 自动追加 \text）
```
然后面板点 **Refresh Registry** → 卡片出现 → **Mount**。

### 4.2 从分发的 zip 安装
把 `my-plugin-1.0.0.zip` 解压到 `plugins\` 下（得到 `plugins\my-plugin\`）→ 面板 Refresh → Mount。
新版覆盖：用**指针包**（`versions\<版本>\` + 顶层 `currentVersion`），可直接在面板 **切换/回滚**，无需重启 Creo。

### 4.3 卸载
卡片 **Unmount**（干净卸载会显示 `R2 terminate: clean`）；需要删除文件则手动删目录后 Refresh。

---

## 5. 用「助手」对话（可选，需要模型）

> 助手 = 面板里的对话式操作入口。它通过一个**本地小服务 `hub-agent`** 调用你指定的模型，模型再通过 hub 执行命令。模型可以是内网部署的 DeepSeek V4 flash，也可以是本地/公网其它 OpenAI 兼容服务。

### 5.1 启动与配置（推荐在面板内完成）

1. 双击 `agent\start_agent.cmd`（只启动一次，窗口保持打开即可）；
2. 面板 →「助手」→ 点 **模型设置** → 填 `baseURL`/模型名/（可选）key → 点 **保存并测试**；
   看到"连接成功，可以开始对话"即可直接对话。
   - 也可离线用 `agent\configure.cmd` 生成配置（二选一，效果相同）；
   - 若 baseURL 填成 `127.0.0.1:8765`（**面板端口，不是模型**）会被拒绝并提示——这正是"失败：method not allowed"的最常见原因。
3. 示例对话：
   - "列出所有插件的状态"
   - "挂载 demo3 并调用 demo3.longtask，等待完成并汇报结果"

> 前提：agent 必须处于运行状态（面板会显示"hub-agent 未运行"）。浏览器不能直接启动本机程序，所以 agent 需要双击启动或由安装器配置开机自启（可选）。

### 5.2 支持的模型接入方案（任选其一）

| # | 方案 | baseURL 示例 | 模型名 | tools 支持 | 建议 toolMode |
|---|---|---|---|---|---|
| 1 | **内网 vLLM/SGLang（推荐，DeepSeek V4 flash）** | `http://<内网IP>:8000/v1` | `deepseek-v4-flash` | 支持 | `native` |
| 2 | 内网 OpenAI 兼容网关 | `http://<网关IP>:<端口>/v1` | 按网关 | 视网关 | `native`/`json` |
| 3 | Ollama（本机/内网） | `http://127.0.0.1:11434/v1` | `qwen2.5:7b` 等 | 部分模型支持 | `json` 兜底 |
| 4 | LM Studio | `http://127.0.0.1:1234/v1` | 已加载模型名 | 支持 | `native` |
| 5 | DeepSeek 公网 API（需联网+key） | `https://api.deepseek.com/v1` | `deepseek-chat` | 支持 | `native` |

**可行性说明**：五种的客户端代码完全一致（都是 OpenAI `/chat/completions`）；差异只在 (a) 是否支持 function calling → 用 `toolMode` 切换；(b) apiKey 有无；(c) baseURL 是否带 `/v1`。`toolMode=json` 时模型只需输出 `{"tool":"...","args":{...}}` 或 `{"final":"..."}`，**不依赖**服务端 tools 能力。

### 5.3 安全与边界（务必知道）
- 令牌：助手请求需 `X-Hub-Token`（=hub_token.txt）；CORS 只允许本机面板；
- **API key 只存在 `agent\agent.config.json`**（不提交、不入日志）；hub DLL 永不联网；
- confirm/destructive 命令仍需**人在面板批准**；助手会把"待批准"提示给你，批准后同参数重试；
- 助手不生成几何，只调用插件声明的确定性命令。

---

## 6. 用 MCP 桥接（可选：DSH / Claude / Cursor）

安装包 win10 档内含 `bridge\`（`creo-hub-mcp`，也可单独用 `creo-hub-mcp-1.0.0.zip`）：
把 `deploy\dsh_mcp_patch.yml` 片段合并进 DSH 的 `cordis.patch.yml`（或按 `bridge\README.zh.md` 配置 Claude/Cursor），即可让支持 MCP 的宿主直接看到 hub 工具面。桥与助手可同时使用，互不影响。

---

## 7. 常见问题（按症状查）

| 症状 | 原因/处理 |
|---|---|
| Creo 里找不到 `creo-hub` 菜单 | ①确认 `config.pro` 中有 `PROTKDAT ...creo-hub.dat`（用安装器 `deploy\doctor.cmd` 体检）；②`config.pro` 必须在 **Creo 启动目录**（快捷方式"起始位置"），常见 `C:\Users\Public\Documents`；③改完需重启 Creo |
| 面板打不开 / 401 | 用菜单打开（令牌随 URL 交付）；hub 重启后旧令牌失效，重新从菜单打开 |
| 装了但插件不出现 | 包必须在 `<安装目录>\plugins\<id>\` 且含合法 `plugin.json`；面板 Refresh；INVALID 时悬停看原因 |
| 防火墙弹窗 | hub 只绑 `127.0.0.1`，正常可拒绝；若弹窗请允许"专用网络"或直接阻止（不影响使用） |
| confirm 命令一直不执行 | 需人在面板确认抽屉点"批准"（60 秒内），然后重试同一命令 |
| 助手连不上 | 先启动 `agent\start_agent.cmd`；面板显示"hub-agent 未运行"即未启动 |
| 助手报"失败：method not allowed" | baseURL 填成了 **hub 面板端口**（8765~8775），不是模型地址；在面板「模型设置」改成模型服务地址（保存时会校验并拒绝面板端口） |
| 助手报 `LLM_*` | `agent.config.json` 的 baseURL/model/key 不对，或内网模型服务未启动；`agent\agent.log` 有结构化原因 |
| 模型不支持 tools（400/无 tool_calls） | 把 `toolMode` 改成 `"json"`（见 §5.2） |
| 升级后还是旧版本 | 关 Creo → 覆盖安装（安装器会备份旧 DLL 为 `.bak`）→ 启动 Creo → `deploy\doctor.cmd` 核对 `hubVersion` |
| Win7 能用吗 | 能：安装包支持 Win7 SP1 x64；win7 档不含 Node/桥/助手（需要对话时另装 Node） |

---

## 8. 更新、卸载与回退

- **更新**：关闭 Creo → 运行新版安装包（覆盖安装）→ 启动 Creo → `deploy\doctor.cmd` 核对；
- **回退**：安装器保留旧 `creo_hub.dll` 为 `.bak-<版本>`，复制回 `build\out\` 即可；
- **卸载**：控制面板"卸载 creo-hub"（或 `deploy\uninstall.cmd`）；`--purge` 才删除整个目录（插件与日志会一起删，慎用）。

---

## 9. 目录一览（装完之后）

```
<安装目录>\
├── build\out\creo_hub.dll     ← hub 本体（Creo 里运行）
├── web\index.html             ← 面板（零 CDN，离线）
├── hub_text\                  ← 面板/菜单文本资源
├── plugins\<id>\              ← 插件包（放这里即可被管理）
├── deploy\install|uninstall|doctor.cmd   ← 安装/卸载/体检（绿色部署用）
├── creo-hub.dat               ← 注册文件（config.pro 的 PROTKDAT 指向它）
├── hub_token.txt              ← 面板令牌（每次启动轮换，勿外传）
├── bridge\  agent\            ← 可选：MCP 桥 / 对话助手（win10 档）
└── docs\                      ← 本教程与开发指南
```

---

## 10. 关键配置速查

**config.pro（Creo 启动目录）**
```
PROTKDAT <安装目录>\creo-hub.dat     # 唯一需要的一行（安装器自动加）
```
**agent\agent.config.json**
```json
{
  "baseURL": "http://10.0.0.5:8000/v1",
  "apiKey": "内网无鉴权则留空",
  "model": "deepseek-v4-flash",
  "temperature": 0.2,
  "maxRounds": 6,
  "maxTokens": 1024,
  "jobPollMs": 60000,
  "toolMode": "native"
}
```

---

> 开发插件？看 `docs\18_插件开发者指南.md`（六模块骨架、SDK v2、契约与验收链）。
> 装机排障？先跑 `deploy\doctor.cmd`，它会把文件/注册/备份/端口/版本逐项写成 `doctor_report.txt`。
