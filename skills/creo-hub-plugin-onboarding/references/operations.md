# operations.md —— creo-hub 插件接入命令参考（creo-hub-plugin-onboarding 附表）

> 所有命令在仓库根 `<REPO_ROOT>` 执行；`%PTK%`=`%CREO_TOOLKIT_ROOT%`。

## 1. 骨架生成（plugin_init）

```
node tools\plugin_init\plugin_init.mjs plugins\my-pkg --id my-pkg --name "My Package" --cmd "get_state:safe;set_fov:confirm:fov:number"
```

- 问答模式（不带 --cmd）：逐条问命令名/risk/参数；`--non-interactive` 全走参数；`--menu` 生成菜单注册模板；`--skip-lint` 跳过生成后验证。
- 依赖声明（M4a）：`--requires dep1,dep2 --provides <id>.<cmd>`（各 ≤8；校验 id/命令键格式；禁自引用）；不传则交互追问。
- 命令规格：`name:risk[:key:type,...]`，多命令用 `;` 分隔；type ∈ string|number|boolean。
- 生成物：六模块+`test\<id>_selftest.c`+README（含编译行）+bin 占位 dll（lint 即过）。
- 生成器自检：`test\run_plugin_init_check.cmd`（六段门：生成→requires 生成/负路径→编译→lint→D-W7→自测）。

## 2. 编译（Win7 纪律 flags 逐字，勿改）

```
cl /nologo /LD /O2 /W3 /MT /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /D_USRDLL /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\includes" /Isrc\hub\vendor\cjson /Isdk /Imy-pkg\src my-pkg\src\my_pkg_main.c my-pkg\src\my_pkg_cmds.c my-pkg\src\my_pkg_adapter.c sdk\hub_sdk.c src\hub\vendor\cjson\cJSON.c /Fotest\out\my-pkg\ /Femy-pkg\bin\my-pkg.dll /link /MACHINE:X64 /SUBSYSTEM:WINDOWS /DEF:my-pkg\my-pkg.def /LIBPATH:"%PTK%\x86e_win64\obj" protk_dll_NU.lib ucore.lib udata.lib netapi32.lib mpr.lib ws2_32.lib psapi.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib winspool.lib bcrypt.lib
```

- id 含连字符时 C 文件名/标识符用下划线（生成器自动转换）。
- 固定三件：hub_sdk.c+cJSON.c+/MT flags。C4828 警告无害。
- **MFC 工程**（VS .vcxproj）：UseOfMfc=Static+RuntimeLibrary=MultiThreaded(/MT)+PTK 库改 **protk_dll_NU.lib**（非 /MD 变体 protk_dllmd_NU.lib）；按文件注入 /utf-8（`<AdditionalOptions>/utf-8 %(AdditionalOptions)</AdditionalOptions>`）。

## 3. 离线验证（三条命令）

```
node test\manifest_lint.mjs plugins            # 清单预检（JSON 禁 BOM，见 SKILL.md §5.1）
test\w7_check.cmd my-pkg\bin\my-pkg.dll        # Win7 静态纪律（D-W7 PASS）
test\run_plugin_selftest.cmd my-pkg            # 自测断言+lint+w7+验收链清单（一条命令）
node tools\plugin_pack\plugin_pack.mjs my-pkg  # 打包（lint→zip→sha256→索引；分发才需要）
```

- 自测范围=命令包装契约层；业务实现后同步改 `test\<id>_selftest.c` 断言。

## 4. 真机接入（开 Creo+creo_hub 后）

1. 面板 **Refresh Registry** → 包出现（INVALID 会带原因）→ **Mount**（弹窗点允许）；
2. 冒烟：`<id>.status` 实调 200；或 token-HTTP：
   ```
   POST http://127.0.0.1:8765/api/v1/call
   头 X-Hub-Token: <hub_token.txt 内容>
   {"plugin":"my-pkg","command":"my-pkg.status"}
   ```
3. confirm/destructive 命令：428 `needConfirm` → 面板抽屉批准（**ISSUED 60s 注意力窗内点批准**）→ **同参重试** → 执行；拒绝则重新发票；批准后 10min 宽限窗（S1b）不急重试；
4. 审计核对（日志区）：`CONFIRM-ISSUE`→`confirm_approve`→`op=call ... txn=… result=ok` 配对；
5. **Unmount** → `P2 ledger drained=N residual=0`+`R2 terminate: clean (0 armed callbacks)`（后者需 §2.2 审计钩）；
6. 新包菜单按钮需 hub 重启（Aux Apps 停止/启动）才出现。

## 5. 版本化热换（指针包，L14 实证）

### 5.1 结构
```
plugins\my-pkg\
├── plugin.json          ← thin pointer: {manifestVersion, id, adapter, currentVersion:"0.1.0"}
│                         **adapter 必填**（缺→INVALID"adapter missing"）
└── versions\0.1.0\      ← 全量清单（id 一致）+bin\+text\
    └── versions\0.2.0\  ← 新版本
```

### 5.2 操作
```
POST /api/v1/update    {"id":"my-pkg","version":"0.2.0"}   # 会话内热换
POST /api/v1/rollback  {"id":"my-pkg","version":"0.1.0"}   # 回滚（需显式 version）
```
- 面板 `currentVersion` 翻转可视；命令面跨版本连续可用（status 全 200）；
- **JSON 无 BOM 写法**（PowerShell）：`[IO.File]::WriteAllText($path, $content, (New-Object Text.UTF8Encoding($false)))`。

## 6. 依赖链（requires/provides，L15 实证）

- 清单加 `requires:["dep1"]`/`provides:["my-pkg.api"]`（各 ≤8 条；plugin_init 可用 `--requires/--provides` 直接生成，也可手工补）；
- **语义**：requires=声明依赖（拓扑排序+排空级联卸载），**非自动挂载闭包**（mount 不带出依赖包）；
- 验证三连：拓扑挂载（依赖序 mount）→排空级联卸载（unmount 带走依赖此包的 ACTIVE 包，反拓扑 drainedList）→`waitingFor` 徽章（requires 包不在 ACTIVE 时亮）。

## 7. 故障排查速查

| 症状 | 原因/处置 |
|---|---|
| lint `entry file not found` | bin\<id>.dll 未编译（占位被删/路径错） |
| lint `plugin.json parse error (syntax)` | **BOM**（239,187,191）或 JSON 语法错——用 §5.2 无 BOM 写法 |
| lint `adapter missing` | 指针包 plugin.json 缺 adapter 字段（§5.1） |
| lint `command id '<x>' must start with '<id>.'` | 命令 id 前缀与包 id 不符 |
| mount 被拒 `Mount refused: hubCmdExport missing` | .def 未导出 hub_cmd 或导出名与清单 hubCmdExport 不符 |
| call 422 | schema 值域校验失败——按 details 指针修 args |
| call 409 NONCE_ARGS_MISMATCH | 重试 args 与批准时不同（三元组绑定）——同参重试 |
| rollback `"version (non-empty string) required"` | 缺显式 version 参数（§5.2） |
| Unmount 无 `R2 terminate: clean` 自报行 | 缺 hub_sdk_set_audit 审计钩（SKILL.md §2.2） |
| 自测 exe 0xC0000135 | ucore46.dll 不在 PATH——用 run_plugin_selftest.cmd（已注入） |

## 8. 案例与真源

- 全程案例（真实外部插件，W1~W4 实测+7 则教训）：私有接入案例过程记录；
- 契约真源：`docs\18_插件开发者指南.md`（M4 版：§14.1 SDK v2、§17 打包/安装/分发）；
- M3.5 关账：`docs\36_M3.5实测结论与关账.md`；
- PTK API 选型：skill `creo-ptk-router`。

## 9. async 长任务（M4b）

```
清单：commands[].async = true      （timeoutSec 兼作 ETA 提示；不豁免 risk 闸门）
调用：POST /api/v1/call → 202 {"ok":true,"result":{"jobId":"j-N"},"poll":{"endpoint":"/api/v1/jobs/j-N","intervalMs":500}}
查询：GET /api/v1/jobs/<id>        → {"ok":true,"job":{state,pct,msg,elapsedMs},"result":{...}}
列表：GET /api/v1/jobs?all=1
```

- 命令写法：循环安全点 `if (hub_sdk_time_left_ms(ctx)==0) {诚实收尾}` + `hub_sdk_progress(ctx,pct,msg)`；
- 状态机：`queued/working/completed/failed/cancelled`；排队期超时=cancelled（retryable）；
- 语义：主线程仍被占（非并行）；变更类任务完成后**先查 job 再决定是否重试**；非 202 仍走 4xx 闸门错误。

## 10. 打包与安装（M4 T6/T7）

```
node tools\plugin_pack\plugin_pack.mjs plugins\my-pkg [--out dist\packages] [--no-lint]
deploy\install.cmd   --root <hub根> --creo-home <Creo 配置目录> [--mode full|core] [--silent] [--yes]
deploy\uninstall.cmd --root <hub根> --creo-home <...> [--purge]
deploy\doctor.cmd    --root <hub根> --creo-home <...> [--deep]
```

- pack 产物：`<id>-<version>.zip`（解压到 `<hub根>\plugins\`）+ `packages.json` 索引（sha256/requires/files）；同内容同 sha256（确定性）；
- install：生成 `creo-hub.dat` + config.pro 标记块（首次备份 `.creo-hub-bak`）；幂等；卸载=精确移除标记（不还原备份）；
- 升级：关 Creo→备份旧 `creo_hub.dll`→覆盖→重启→doctor 校验；失败=手动回 `.bak`；
- 注册机制待 M4c 实测定案（config.pro 选项 vs .dat 自动发现）。
