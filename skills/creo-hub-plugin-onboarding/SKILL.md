---
name: "creo-hub-plugin-onboarding"
description: "creo-hub 插件接入技能：把 Creo 6 Pro/TOOLKIT 功能插件接入 creo-hub 生态（清单契约、hub_sdk 命令面、/call 执行链、面板审批、桥工具面、async 长任务进度、打包(zip+sha256)、一键安装/卸载/doctor、版本化热换、依赖链调和）的完整操作方案。凡涉及 plugin.json v2 声明、hub_cmd/hub_sdk、命令面设计（risk/inputSchema/descForLLM/async）、打包 plugins\\<id>、接入验收（挂载/冒烟/批准链/审计/R2 clean）、plugin_init 脚手架、接入测试、指针包热换、requires 依赖链、分发部署时调用本 skill。"
---

# creo-hub 插件接入操作方案（creo-hub-plugin-onboarding）

把一个 Creo PTK 功能插件接入 creo-hub = 给插件装上**命令面**（/call 可执行、模型可调、面板可批）。目标形态：单 DLL 双面（原 UI 菜单零改动 + hub_cmd 导出），接入后命令自动成为 MCP 一级工具。

**仓库**：`<REPO_ROOT>`（下述相对路径均以此为根）。**开发指南真源**：`docs\18_插件开发者指南.md`（§14.0b 六模块方案、§9 红线六条）；**案例样本**：一个真实外部插件的接入过程记录（W1~W4 实测 + 7 则案例教训，私有文档）。

## 0. 红线六条（违反=崩溃或闸门失效，详见 docs\18 §9）

1. `user_initialize` 内禁止模态对话框（用 ProUIDialogShowAsync）；
2. `user_terminate` 返回前注销**全部** PTK 回调 + `hub_sdk_terminate_check` 自报（**必须先 `hub_sdk_set_audit` 挂审计钩**——W2 发现 #1：无钩→自报静默消失）；
3. DLL 用 `/MT` 静态 CRT + `/D_WIN32_WINNT=0x0601` 编译（Win7 纪律；MFC 工程用 UseOfMfc=Static+protk_dll_NU.lib）；
4. risk/reversible/idempotent 声明必须诚实（闸门与 504 retryable 直接消费）；
5. `timeoutSec` 克制（主线程同步=整 hub 阻塞面，建议 ≤5s）；
6. 文本资源放 `textDir\text\usascii\`（Creo 自动追加 `\text`；勿用中文路径）。

## 1. 接入流水线（8 步，照序执行）

| 步 | 动作 | 命令/工具 |
|---|---|---|
| 1 | 定包身份（id/命令清单/风险声明） | 问答或直接进 2 |
| 2 | 生成六模块骨架 | `node tools\plugin_init\plugin_init.mjs <目标> --id <id> --cmd "name:risk[:arg:type,...]"` |
| 3 | 填业务代码（每命令一函数） | 见 §2 命令函数契约 |
| 4 | 编译 | 见 `references/operations.md` 编译行（flags 逐字；MFC 工程注意 /MT） |
| 5 | 清单预检 | `node test\manifest_lint.mjs plugins` 全绿（**JSON 禁 BOM**，见 §5.1） |
| 6 | Win7 纪律自检 | `test\w7_check.cmd <包>\bin\<id>.dll` → D-W7 PASS |
| 7 | 离线自测 | `test\run_plugin_selftest.cmd <包>` 一条命令全链 |
| 8 | 真机接入验收 | 面板 Refresh→Mount→冒烟→批准链→审计→Unmount R2 clean（清单见 §4） |
| 9 | 打包（可选，分发才需要） | `node tools\plugin_pack\plugin_pack.mjs plugins\<id>` → `dist\packages\<id>-<ver>.zip`+SHA256+索引（§8） |
| 10 | 部署（可选） | 部署机 `deploy\install.cmd --root <hub根> --creo-home <配置目录>`；体检 `deploy\doctor.cmd`（§8） |

一键自证（2~7 压缩）：`test\run_plugin_init_check.cmd` 六段门（生成→编译→lint→D-W7→自测 + requires/provides 生成与负路径）。

## 2. 六模块骨架（生成器产出，开发者只改三处）

| 模块 | 文件 | 开发者做什么 |
|---|---|---|
| M1 清单 | plugin.json | 补 descForLLM（写给模型：前置条件/参数含义/失败语义——**自愈率取决于它**） |
| M2 生命周期 | src\<id>_main.c | 需要菜单时补注册；terminate 红线照抄；**hub_sdk_set_audit 挂钩** |
| M3 命令实现 | src\<id>_cmds.c | **业务本体**：每命令一函数，签名固定 `cJSON* fn(const cJSON* args, HubCmdCtx* ctx, char* hint)` |
| M4 分发导出 | src\<id>_adapter.c + <id>.def | 加命令=表里加一行；hub_cmd 一行转发不动 |
| M5 资源 | text\text\usascii\<id>_messages.txt | 菜单标签 |
| M6 构建 | 编译行 | 抄 README.md 的编译行改路径 |

**命令函数契约**（M3）：成功=返回 result JSON（cJSON 对象，SDK 负责序列化释放）；失败=填 `hint`（写清"哪里错了+怎么改"，模型按 hint 自愈）返回 NULL；**不写通道层错误码**（SDK 内建双层错误纪律）；不返回 `{"ok":false}` 对象（那是 200 业务包络，慎用）。取参用 `hub_sdk_str/num/bool`；摸模型用 `hub_sdk_active_part`；长命令用 `hub_sdk_time_left_ms` 协作让路。

### 2.2 生命周期钩（W2 发现 #1 关键补充）

```c
static void MyAudit(const char* line) {
    OutputDebugStringA(line); /* 或写自有日志 */
    FILE* f = fopen("my_ops.log", "a");
    if (f) { fputs(line, f); fputc('\n', f); fclose(f); }
}
/* user_initialize 里必须挂： */
hub_sdk_set_audit(MyAudit);
/* user_terminate 里自报+落证： */
char report[256];
hub_sdk_terminate_check(report, sizeof(report));
MyAudit(report[0] ? report : "R2 terminate: clean (0 armed callbacks)");
```
**无审计钩→terminate_check 报告静默消失**（W2 实测发现）；hub 侧 P2 台账照常归零，但自证行不可见。

### 2.5 指针包结构（版本化热换，W3 L14 实证）

```
plugins\<id>\
├── plugin.json          ← thin pointer: {manifestVersion, id, adapter, currentVersion:"<v>"}
│                         **adapter 字段必填**（缺→INVALID"adapter missing"）
└── versions\<v>\
    ├── plugin.json      ← 全量清单（id 必须与指针一致）
    ├── bin\<id>.dll
    └── text\text\usascii\...
```

- 指针只重定向一层；`POST /update`/`/rollback`=切指针+排空级联重挂；
- **rollback 需显式 version 参数**：`{"id":"<id>","version":"<v>"}`（缺→`"version (non-empty string) required"`）；
- plugin_init 现已支持生成 requires/provides（`--requires a,b --provides <id>.<cmd>`，见 §6）。

## 3. 清单声明纪律（plugin.json v2）

- 命令 id 必须带 `<id>.` 前缀；commands ≤16；inputSchema 只用白名单关键字，序列化 ≤1023 字节；
- `risk`：safe=只读无副作用 / confirm=改状态可谨慎重试 / destructive=不可逆——**如实填**，destructive 恒面板批准；
- `timeoutSec` 诚实克制；`context` part|assembly|any（仅声明透出）；
- 生成器自动生成 `<id>.status` 只读诊断命令（铁律 2 最小命令，保留）。

## 4. 验收链（每包必过，一条不落）

1. lint 全绿；2. D-W7 PASS；3. 面板 Refresh→Mount（弹窗）；4. safe 命令实调 200（连通冒烟）；5. confirm/destructive 各走一遍 428→面板批准→**同参重试**→执行（**ISSUED 60s 注意力窗**：票签发前先就位；批准后 10min 宽限窗不急）；6. 审计行配对（txn 贯穿 confirm_approve→call）；7. Unmount 见 `P2 ledger drained=N residual=0`（hub 侧台账归零）+`R2 terminate: clean`（自报行，需 §2.2 审计钩）。

## 5. 常见坑（W1~W4 实测教训）

### 5.1 BOM 陷阱（W3 教训，必踩级）
PowerShell `Set-Content -Encoding UTF8` 写 **BOM**（字节 239,187,191）→ JSON 解析失败（lint/hub 双卡）。**无 BOM 写法**：
```powershell
[IO.File]::WriteAllText($path, $content, (New-Object Text.UTF8Encoding($false)))
```

### 5.2 其他坑
- 离线跑测试 exe 报 0xC0000135：PTK 运行时 ucore46.dll 不在 PATH——runner 已注入 `Common Files\x86e_win64\lib`；
- 批处理 `if errorlevel 1` 有**符号盲区**（负值 exit 被当成功）——比较 `%ERRORLEVEL%` 字符串；
- 新包的 Creo 菜单按钮需 hub 重启（Aux Apps 停/起）才出现，面板按钮不受限；
- ProSelect 一个会话只安全一次；重选走被动缓冲读选（ProSelbufferSelectionsGet）——接入案例 §1.3 七条 PTK 教训先读；
- 模式条 M1/M2（操作者模式）只改"谁批准"，您的代码零差异。

## 6. 依赖声明（requires/provides，W4 L15 实证）

- `requires`/`provides` 各 ≤8 条字符串；requires 是**声明依赖**（拓扑排序+排空级联卸载），**非自动挂载闭包**（mount 不带出依赖包，需手动拓扑挂载）；
- 语义：挂载=按拓扑序可挂；**排空级联卸载**（unmount 带走依赖此包的 ACTIVE 包，反拓扑排空）；`waitingFor` 徽章=requires 包不在 ACTIVE 时亮；
- 生成：`plugin_init ... --requires dep1,dep2 --provides <id>.api`（校验 id/命令键格式、≤8、禁自引用）；也可生成后手工补。

## 7. async 长任务命令（M4b）

**何时用**：单次执行 >5s 的命令（批量几何、导入导出、长计算）。`timeoutSec` 超时仍不杀主线程，async 只是让**进度与结果可观察**——**不是并行**。

```json
{ "id": "my-pkg.batch", "risk": "confirm", "async": true, "timeoutSec": 60, ... }
```

```c
/* 命令函数内：循环安全点先查预算，再报进度（SDK v2） */
for (i = 0; i < n; i++) {
    if (hub_sdk_time_left_ms(ctx) == 0) { /* 诚实收尾：部分结果或失败 */ }
    do_step(i);
    hub_sdk_progress(ctx, (i + 1) * 100 / n, "step");   /* 1=已写/0=无槽(no-op) */
}
```

- 玩法：`POST /api/v1/call` → **202** `{jobId, poll:{endpoint,intervalMs}}` → `GET /api/v1/jobs/<id>` 查 `state/pct/msg/result`；状态机 `queued/working/completed/failed/cancelled`；
- 诚实边界：任务执行期间主线程仍被占（其他 PTK 命令排队/超时）；**变更类 async 命令完成后勿盲重试**，先查 job；`async` 不豁免 risk 闸门；
- 进度槽契约=`sdk\hub_jobslot.h`（hub 建映射、SDK 写、hub worker 读；无槽=no-op）。

## 8. 打包与安装（M4 T6/T7）

```
node tools\plugin_pack\plugin_pack.mjs plugins\my-pkg            # → dist\packages\my-pkg-<ver>.zip + packages.json
deploy\install.cmd   --root <hub根> --creo-home <Creo 用户配置目录> [--mode full|core] [--silent]
deploy\uninstall.cmd --root <hub根> --creo-home <...> [--purge]
deploy\doctor.cmd    --root <hub根> --creo-home <...> [--deep]   # doctor_report.txt + 退出码
```

- **pack**：先 lint 再打包，产物**确定性**（同内容同 sha256）+ 本地仓库索引（id/version/sha256/requires）；解压到 `<hub根>\plugins\` 即得 `plugins\<id>\`；
- **install**：预检→落位（build\out/web/hub_text/plugins）→生成 `creo-hub.dat`+config.pro 标记块（首次备份 `.creo-hub-bak`）→验证；**幂等**（重复装只有一块）；**可逆**（uninstall 精确移除标记，不还原备份）；
- **升级**：关 Creo→备份旧 `creo_hub.dll`→覆盖→重启→`doctor`；失败=手动回 `.bak`；
- 注册机制（config.pro `toolkit_registry_file` vs `.dat` 自动发现）以 M4c 实测定案为准；实测前用双写+必要时 Aux Applications 手工注册一次。

## 9. 资源索引

| 资源 | 路径 |
|---|---|
| 开发指南（单一真源） | `docs\18_插件开发者指南.md` |
| 接入案例（过程记录样本） | 私有接入案例过程记录（W1~W4 实测 + 案例教训 7 则） |
| 模板包（六模块骨架） | `tools\plugin_tpl\`（README 抄改七步） |
| 脚手架生成器 | `tools\plugin_init\plugin_init.mjs`（--non-interactive 供脚本用） |
| 测试模块 | `test\run_plugin_selftest.cmd <包>` |
| 详细命令参考 | 本 skill `references/operations.md` |
| PTK API 选函数 | skill `creo-ptk-router`（12 域子 skill+函数附表） |
| M3.5 关账文档 | `docs\36_M3.5实测结论与关账.md` |
