# tools\plugin_tpl —— 插件接入模板包（M3e 开发方案代码级落地）

> 指南 §14.0b 六模块的**可复制骨架**；编译门 `test\run_plugin_tpl_check.cmd` 持续验证本模板可编+清单可过 lint+Win7 纪律可过。M3.5 的 `plugin_init` 交互脚手架将以本模板为前身。
> 本目录**不在** `plugins\` 下，hub 注册表不扫描它——它是源码模板，不是已装包。

## 六模块对照（开发者编写面）

| 模块 | 文件 | 抄改要点 |
|---|---|---|
| M1 清单 | `plugin.json` | 改 id/name/commands[]；命令 id 前缀=`<id>.`；risk/timeoutSec/reversible/idempotent 诚实声明（闸门直接消费） |
| M2 生命周期 | `src\tpl_main.c` | 四入口；init 禁模态、terminate 注销全部回调+自报（红线①②）；菜单照 demo3_main.c 抄 |
| M3 命令实现 | `src\tpl_cmds.c/.h` | 每命令一业务函数；成功返 result JSON、失败填 hint 返 NULL；保留≥1 条只读命令（铁律 2） |
| M4 分发导出 | `src\tpl_adapter.c` + `tpl.def` | G_CMDS 末项 NULL 收口；hub_cmd 一行转发；.def 五符号；LIBRARY 名随 dll 改 |
| M5 资源 | `text\text\usascii\<id>_messages.txt` | 改名；目录层级勿动（`\text` 追加陷阱） |
| M6 构建 | 编译门/自有脚本 | flags 逐字（/MT+/utf-8+/D_WIN32_WINNT=0x0601…）；链 protk_dll_NU/ucore/udata；`test\w7_check.cmd <你的dll>` 自检 |

## 抄改七步（改名六处+验收）

1. 复制本目录到 `<hub根>\plugins\<你的包目录>\`（目录名任意，建议=id）；
2. 改 `plugin.json`：id/name/version/commands[]（**id 前缀同步**）；
3. 改 dll 名三处：`plugin.json.entry`、`tpl.def` 的 LIBRARY、构建输出名；
4. 改消息文件名 `text\text\usascii\<id>_messages.txt`；
5. 改源文件里包名常量（tpl_cmds.c 的 "tpl" 串、G_CMDS 命令 id 前缀）；
6. 编译：照 `test\run_plugin_tpl_check.cmd` 的 cl 行改路径（或并入你的构建脚本）；
7. 验收链：`node test\manifest_lint.mjs <hub根>\plugins` 全绿 → 面板 Refresh→Mount（弹窗）→ safe 命令实调 200 →（有 confirm/destructive）走一遍面板批准链 → 审计行落账 → Unmount 见 `R2 terminate: clean`。

## 固定封装边界（你不需要写的）

IN/OUT_JSON 编组、cJSON 生命周期、双层错误纪律、审计投递、R2 登记簿槽位、active_part 解析、args 便捷取值——全在 `sdk\hub_sdk.h/.c`（编入你的 DLL：+`cJSON.c`）。**你永远不碰 ProArgument。**

## 活样例与负面教材

- 全功能活样例：`plugins\demo3`（四命令全风险级+菜单+R2 登记簿+几何门案例）；
- 负面教材：`plugins\demoslot`（terminate 遗留回调→卸载后崩 Creo，**生产勿挂载**）。
