#!/usr/bin/env node
/**
 * tools/plugin_init/plugin_init.mjs —— creo-hub 接入脚手架（M3.5 F3，Q-M3.5-2=A）
 *
 * 用法:
 *   node tools/plugin_init/plugin_init.mjs <目标目录> [选项]
 * 选项:
 *   --id <pkg-id>          包 id（1-63 字符 [a-z0-9-]；缺省交互问答/目录名）
 *   --name <显示名>        清单 name 字段
 *   --cmd <规格列表>       命令规格，分号分隔：name:risk[:arg:type,...]
 *                          risk ∈ safe|confirm|destructive；例：
 *                          --cmd "get_state:safe;set_fov:confirm:fov:number"
 *   --menu                 生成菜单注册模板（默认不生成）
 *   --non-interactive      全部走参数/缺省值（供 CI/测试夹具）
 *   --skip-lint            跳过生成后 lint 验证
 *
 * 设计:
 *   - 单一真源：六模块骨架直接读 tools\plugin_tpl 变换生成（无模板副本，防漂移）；
 *   - 永远生成一条可用只读诊断命令 <id>.status（铁律 2），其余命令=诚实桩
 *     （返回 NULL + hint "scaffold stub not implemented"，绝不假装成功）；
 *   - 生成 bin/<id>.dll 占位文件使 manifest_lint 立即可过（编译后覆盖）；
 *   - 生成后自动 lint（copy 流隔离根），"生成即验证"。
 *
 * 零依赖：仅 node 内置模块（node>=18，与桥生态同基线）。
 */
import { readFileSync, writeFileSync, mkdirSync, cpSync, rmSync, existsSync } from "node:fs";
import { join, resolve, basename, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";
import { createInterface } from "node:readline/promises";

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = resolve(HERE, "..", "..");            // 仓库根
const TPL = join(ROOT, "tools", "plugin_tpl");     // 骨架单一真源
const LINT = join(ROOT, "test", "manifest_lint.mjs");

/* ---------------- 参数解析 ---------------- */
const argv = process.argv.slice(2);
function flag(name) { return argv.includes(name); }
function opt(name, dflt) {
  const i = argv.indexOf(name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : dflt;
}
const targetDir = argv.find(a => !a.startsWith("--") && a !== opt("--id") && a !== opt("--name") && a !== opt("--cmd") && a !== opt("--requires") && a !== opt("--provides"));
if (!targetDir) {
  console.log("usage: plugin_init.mjs <目标目录> [--id <pkg-id>] [--name <显示名>]");
  console.log("                       [--cmd \"name:risk[:arg:type,...;...\"] [--requires a,b] [--provides pkg.cmd,...]");
  console.log("                       [--menu] [--non-interactive] [--skip-lint]");
  process.exit(2);
}
const nonInteractive = flag("--non-interactive");

/* ---------------- 规格收集 ---------------- */
const ID_RE = /^[a-z0-9][a-z0-9-]{0,62}$/;
const NAME_RE = /^[a-z0-9][a-z0-9_-]{0,62}$/;   /* 命令名段：允许下划线 */
const RISKS = ["safe", "confirm", "destructive"];
const ARG_TYPES = { s: "string", string: "string", n: "number", number: "number", b: "boolean", boolean: "boolean" };

function parseCmdSpec(spec) {
  /* name:risk[:key:type,key:type] */
  const parts = spec.split(":");
  const name = (parts[0] || "").trim();
  const risk = (parts[1] || "safe").trim();
  const args = [];
  if (parts.length > 2 && parts[2]) {
    for (const kv of parts[2].split(",")) {
      const seg = kv.split(":");
      const k = (seg[0] || "").trim();
      const t = (seg[1] || "string").trim();
      if (!k || !ARG_TYPES[t]) throw new Error(`bad arg spec: ${kv}`);
      args.push({ key: k, type: ARG_TYPES[t] });
    }
  }
  return { name, risk, args };
}

async function gather() {
  const rl = nonInteractive ? null : createInterface({ input: process.stdin, output: process.stdout });
  const ask = async (q, dflt) => {
    if (!rl) return dflt;
    const a = (await rl.question(`${q}${dflt ? ` [${dflt}]` : ""}: `)).trim();
    return a || dflt;
  };
  let id = opt("--id") || (nonInteractive ? basename(resolve(targetDir)) : await ask("包 id（1-63 [a-z0-9-]）", basename(resolve(targetDir))));
  id = id.toLowerCase();
  if (!ID_RE.test(id)) { console.error(`[ERR] bad id '${id}' (need 1-63 chars of [a-z0-9-])`); process.exit(2); }
  const name = opt("--name") || (nonInteractive ? id : await ask("显示名 name", id));

  let cmds = [];
  const cmdSpec = opt("--cmd");
  if (cmdSpec) {
    for (const s of cmdSpec.split(";")) if (s.trim()) cmds.push(parseCmdSpec(s.trim()));
  } else if (!nonInteractive) {
    for (;;) {
      const n = await ask("命令名（回车结束）", "");
      if (!n) break;
      const r = await ask(`  risk（${RISKS.join("/")}）`, "safe");
      const argsSpec = await ask("  参数（key:type 逗号分隔，无则回车）", "");
      cmds.push(parseCmdSpec(`${n}:${r}${argsSpec ? ":" + argsSpec : ""}`));
    }
  }
  for (const c of cmds) {
    if (!NAME_RE.test(c.name)) { console.error(`[ERR] bad command name '${c.name}'`); process.exit(2); }
    if (!RISKS.includes(c.risk)) { console.error(`[ERR] bad risk '${c.risk}' (need ${RISKS.join("|")})`); process.exit(2); }
  }

  /* M4a A4: requires/provides（docs\39 §5.2；lint 规则=数组 ≤8 字符串） */
  const parseList = (v) => (v ? v.split(",").map(s => s.trim()).filter(Boolean) : []);
  let requires = parseList(opt("--requires"));
  let provides = parseList(opt("--provides"));
  if (!nonInteractive) {
    if (opt("--requires") === undefined)
      requires = parseList(await ask("依赖插件 id（逗号分隔，无则回车）", ""));
    if (opt("--provides") === undefined)
      provides = parseList(await ask("提供命令键（逗号分隔，无则回车）", ""));
  }
  for (const r of requires) {
    if (!ID_RE.test(r)) { console.error(`[ERR] bad requires entry '${r}' (need plugin id [a-z0-9-])`); process.exit(2); }
    if (r === id) { console.error("[ERR] requires must not contain the package's own id"); process.exit(2); }
  }
  for (const p of provides) {
    if (!/^[a-z0-9][a-z0-9-]*\.[a-z0-9][a-z0-9_-]*$/.test(p)) {
      console.error(`[ERR] bad provides entry '${p}' (need '<plugin-id>.<cmd>')`); process.exit(2);
    }
  }
  if (requires.length > 8 || provides.length > 8) {
    console.error("[ERR] requires/provides each allow at most 8 entries");
    process.exit(2);
  }

  const menu = flag("--menu");
  if (rl) rl.close();
  return { id, name, cmds, menu, requires, provides };
}

/* ---------------- 生成 ---------------- */
function cident(id) { return id.replace(/-/g, "_"); }   /* C 标识符化 */

function genManifest(sp) {
  const commands = [];
  commands.push({
    id: `${sp.id}.status`,
    title: `${sp.name} status (read-only)`,
    desc: "只读诊断：返回包名/SDK 版本/执行预算（铁律 2 的最小命令）。",
    descForLLM: "Read-only, no side effects, safe to retry. Returns {package, sdk_version, budget_ms}. Use as the connectivity smoke test after mount.",
    risk: "safe",
    inputSchema: { type: "object", properties: {} },
    timeoutSec: 3,
    context: "any",
    reversible: true,
    idempotent: true
  });
  for (const c of sp.cmds) {
    const props = {};
    for (const a of c.args) props[a.key] = { type: a.type, description: `${a.key} (${a.type})` };
    commands.push({
      id: `${sp.id}.${c.name}`,
      title: `${sp.name}.${c.name}`,
      desc: `TODO：命令说明（scaffold 生成，业务实现后更新）`,
      descForLLM: `TODO: describe preconditions, argument meanings and failure semantics for the model (scaffold stub; command ${sp.id}.${c.name} not implemented yet).`,
      risk: c.risk,
      inputSchema: { type: "object", properties: props, ...(c.args.length ? { required: c.args.map(a => a.key) } : {}) },
      timeoutSec: 5,
      context: "any",
      reversible: c.risk !== "destructive",
      idempotent: true
    });
  }
  return JSON.stringify({
    manifestVersion: 2, id: sp.id, name: sp.name, version: "0.1.0",
    adapter: "ptk-sync", entry: `bin/${sp.id}.dll`, textDir: "text",
    ...(sp.requires && sp.requires.length ? { requires: sp.requires } : {}),
    ...(sp.provides && sp.provides.length ? { provides: sp.provides } : {}),
    commands
  }, null, 2) + "\n";
}

function genCmdsH(sp) {
  const ci = cident(sp.id);
  let s = `#ifndef ${ci.toUpperCase()}_CMDS_H\n#define ${ci.toUpperCase()}_CMDS_H\n/* M3 命令实现模块（plugin_init 生成；指南 §14.0b 模块 3）。 */\n#include "cJSON.h"\n#include "hub_sdk.h"\n\ncJSON* ${ci}_cmd_status(const cJSON* args, HubCmdCtx* ctx, char* hint);\n`;
  for (const c of sp.cmds) s += `cJSON* ${ci}_cmd_${c.name.replace(/-/g, "_")}(const cJSON* args, HubCmdCtx* ctx, char* hint);\n`;
  return s + "\n#endif\n";
}

function genCmdsC(sp) {
  const ci = cident(sp.id);
  /* 状态命令=可用实现（直读 tpl_cmd_status 变换）；其余=诚实桩 */
  const tplCmds = readFileSync(join(TPL, "src", "tpl_cmds.c"), "utf8");
  let statusFn = tplCmds.slice(tplCmds.indexOf("/* 铁律 2"));
  statusFn = statusFn.replace(/\btpl_cmd_status\b/g, `${ci}_cmd_status`)
                     .replace(/"tpl"/g, `"${sp.id}"`)
                     .replace(/\btpl_audit\b/g, `${ci}_audit`)
                     .replace(/\/\* 走读占位[\s\S]*?\*\//, "");
  let s = `/* =====================================================================\n * ${sp.id} - M3 命令实现模块（plugin_init 生成，指南 §14.0b 模块 3）\n * 成功=返回 result JSON；失败=填 hint（写给模型自愈）返回 NULL。\n * 状态命令为可用实现；其余命令=诚实桩（不假装成功），业务实现后更新。\n * ===================================================================== */\n#include <ProToolkit.h>\n#include "cJSON.h"\n#include "hub_sdk.h"\n#include "${ci}_cmds.h"\n\n`;
  s += statusFn.trim() + "\n\n";
  for (const c of sp.cmds) {
    const fn = `${ci}_cmd_${c.name.replace(/-/g, "_")}`;
    s += `cJSON* ${fn}(const cJSON* args, HubCmdCtx* ctx, char* hint)\n{\n`;
    for (const a of c.args) {
      if (a.type === "number")        s += `    double ${a.key} = hub_sdk_num(args, "${a.key}", 0);\n`;
      else if (a.type === "boolean")  s += `    int ${a.key} = hub_sdk_bool(args, "${a.key}", 0);\n`;
      else                            s += `    const char* ${a.key} = hub_sdk_str(args, "${a.key}");\n`;
    }
    s += `    (void)args; (void)ctx;\n    _snprintf(hint, HUB_SDK_HINT_SZ, "scaffold stub not implemented: ${sp.id}.${c.name} (fill ${fn})");\n    return NULL;   /* 诚实桩：业务实现前不假装成功 */\n}\n\n`;
  }
  return s;
}

function genAdapter(sp) {
  const ci = cident(sp.id);
  let s = `/* =====================================================================\n * ${sp.id} - M4 分发与导出模块（plugin_init 生成，铁律 3；指南 §14.0b 模块 4）\n * ===================================================================== */\n#include <ProToolkit.h>\n#include "hub_sdk.h"\n#include "${ci}_cmds.h"\n\nstatic const HubCmdEntry G_CMDS[] = {\n    { "${sp.id}.status", ${ci}_cmd_status },\n`;
  for (const c of sp.cmds) s += `    { "${sp.id}.${c.name}", ${ci}_cmd_${c.name.replace(/-/g, "_")} },\n`;
  s += `    { NULL, NULL }\n};\n\nPRO_TK_DLL_EXPORT ProError hub_cmd(ProArgument* in, ProArgument** out)\n{\n    return hub_sdk_entry(in, out, G_CMDS);\n}\n`;
  return s;
}

function genMain(sp) {
  const ci = cident(sp.id);
  let s = readFileSync(join(TPL, "src", "tpl_main.c"), "utf8");
  s = s.replace(/\btpl_audit\b/g, `${ci}_audit`)
       .replace(/\btpl_cmd_status\b/g, `${ci}_cmd_status`)
       .replace(/tools\/plugin_tpl/g, `${sp.id} (plugin_init 生成)`)
       .replace(/\btpl\b/g, sp.id)
       .replace(/\bTPL\b/g, sp.id.toUpperCase());
  /* 菜单模板（--menu 才保留注册提示，本体零 UI 耦合） */
  if (!sp.menu) s = s.replace(/    \/\* 菜单\/动作注册写在这里[\s\S]*?version 指向 Creo 版本串，可用于最低版本自检。 \*\/\n/, "    /* 无菜单包（--menu 可生成注册模板） */\n");
  return s;
}

function genDef(sp) {
  return `LIBRARY ${sp.id}\nEXPORTS\n  user_initialize\n  user_terminate\n  user_start\n  user_stop\n  hub_cmd\n`;
}

function genSelftest(sp) {
  /* plugin_test_kit（M-4）：离线自测件——命令包装契约层（不跑 PTK 业务路径）。
   * 生成即具断言力：status=必须返回结果；诚实桩=必须 NULL+hint 已填。 */
  const ci = cident(sp.id);
  let s = `/* =====================================================================\n * ${sp.id} - 离线自测件（plugin_init 生成，plugin_test_kit M-4）\n * 范围=命令包装契约层：成功路径返回 result、失败路径填 hint 返 NULL。\n * 业务实现后请同步更新本文件断言（stub 断言会随实现失效）。\n * 一键跑： test\\run_plugin_selftest.cmd <本包目录>\n * ===================================================================== */\n#include <stdio.h>\n#include <string.h>\n#include <ProToolkit.h>\n#include "cJSON.h"\n#include "hub_sdk.h"\n#include "${ci}_cmds.h"\n\nstatic int g_pass = 0, g_fail = 0;\nstatic void check(int ok, const char* msg)\n{\n    if (ok) { g_pass++; printf("  [PASS] %s\\n", msg); }\n    else    { g_fail++; printf("  [FAIL] %s\\n", msg); }\n}\n\nint main(void)\n{\n    char hint[HUB_SDK_HINT_SZ];\n    HubCmdCtx ctx;\n    cJSON* r;\n    memset(&ctx, 0, sizeof(ctx));\n\n    printf("--- ${sp.id} offline selftest ---\\n");\n\n    /* ${sp.id}.status：可用实现必须返回结果 JSON */\n    memset(hint, 0, sizeof(hint));\n    r = ${ci}_cmd_status(NULL, &ctx, hint);\n    check(r != NULL, "status: returns result JSON");\n    if (r) { check(cJSON_GetObjectItem(r, "package") != NULL, "status: result has package field"); cJSON_Delete(r); }\n`;
  for (const c of sp.cmds) {
    const fn = `${ci}_cmd_${c.name.replace(/-/g, "_")}`;
    s += `\n    /* ${sp.id}.${c.name}：诚实桩契约=返回 NULL 且 hint 已填（业务实现后改断言） */\n`;
    s += `    memset(hint, 0, sizeof(hint));\n`;
    s += `    r = ${fn}(NULL, &ctx, hint);\n`;
    s += `    check(r == NULL && hint[0] != '\\0', "${sp.id}.${c.name}: honest stub -> NULL + hint filled");\n`;
    s += `    if (r) cJSON_Delete(r);\n`;
  }
  s += `\n    printf("== ${sp.id} selftest: %d pass, %d fail ==\\n", g_pass, g_fail);\n    return g_fail ? 1 : 0;\n}\n`;
  return s;
}

function genMessages(sp) {
  return `# ${sp.id} message file（plugin_init 生成；M5 资源模块）\n# 加菜单时定义标签： <MSGNAME> <label>\n# 目录纪律：必须位于 textDir\\text\\usascii\\（Creo 自动追加 \\text）\n${sp.id.toUpperCase()}_MENU ${sp.name}\n`;
}

function genBuildReadme(sp) {
  const ci = cident(sp.id);
  return `# ${sp.id} —— creo-hub 接入包（plugin_init 生成）

> 生成自 tools\\plugin_init（骨架单一真源=tools\\plugin_tpl）。指南=docs\\18 §14.0b。

## 六模块对照

| 模块 | 文件 | 状态 |
|---|---|---|
| M1 清单 | plugin.json | 已生成（descForLLM 的 TODO 待补写——自愈率取决于它） |
| M2 生命周期 | src\\${ci}_main.c | 已生成（红线注释在内） |
| M3 命令实现 | src\\${ci}_cmds.c/.h | ${sp.id}.status=可用实现；其余=诚实桩（**先填业务再上线**） |
| M4 分发导出 | src\\${ci}_adapter.c + ${sp.id}.def | 已生成（加命令=表里加行） |
| M5 资源 | text\\text\\usascii\\${sp.id}_messages.txt | 已生成 |
| M6 构建 | 下方编译行 | 参数逐字（Win7 纪律） |

## 编译（Win7 纪律 flags，链库表同 build.cmd v4）

\`\`\`
cl /nologo /LD /O2 /W3 /MT /utf-8 /D_WINDOWS /D_WIN32_WINNT=0x0601 /DNTDDI_VERSION=0x06010000 /DNDEBUG /D_USRDLL /DUNICODE /D_UNICODE /DPRO_MACHINE=29 /DPRO_OS=4 /D_CRT_SECURE_NO_WARNINGS /DCJSON_HIDE_SYMBOLS /I"%PTK%\\includes" /Isrc\\hub\\vendor\\cjson /Isdk /I${sp.id}\\src ${sp.id}\\src\\${ci}_main.c ${sp.id}\\src\\${ci}_cmds.c ${sp.id}\\src\\${ci}_adapter.c sdk\\hub_sdk.c src\\hub\\vendor\\cjson\\cJSON.c /Fotest\\out\\${sp.id}\\ /Fe${sp.id}\\bin\\${sp.id}.dll /link /MACHINE:X64 /SUBSYSTEM:WINDOWS /DEF:${sp.id}\\${sp.id}.def /LIBPATH:"%PTK%\\x86e_win64\\obj" protk_dll_NU.lib ucore.lib udata.lib netapi32.lib mpr.lib ws2_32.lib psapi.lib user32.lib gdi32.lib advapi32.lib shell32.lib ole32.lib winspool.lib bcrypt.lib
\`\`\`

（bin\\${sp.id}.dll 现为 lint 占位文件，编译输出覆盖之。）

## 依赖声明（可选）

- \`requires\`=**装载序依赖**（拓扑挂载 + 排空级联卸载；**不是自动挂载闭包**——M3.5 W4 语义）；
- \`provides\`=本包装入命令注册表的键（\`<id>.<cmd>\`，docs\\12 义；与"服务键"不同义）。
- 本包：requires=[${(sp.requires || []).join(", ") || "—"}]，provides=[${(sp.provides || []).join(", ") || "—"}]
- 改依赖：直接编辑 plugin.json 的 requires/provides（各 ≤8），再跑 lint。

## 验收链

1. \`node test\\manifest_lint.mjs plugins\` 全绿
2. \`test\\w7_check.cmd ${sp.id}\\bin\\${sp.id}.dll\` → D-W7 PASS
3. 面板 Refresh → Mount → ${sp.id}.status 实调 200
4. （有 confirm/destructive）428 → 面板批准 → 同参重试 → 执行
5. 审计行配对（txn 贯穿 confirm_approve→call）
6. Unmount → \`R2 terminate: clean\`
`;
}

async function main() {
  const sp = await gather();
  const out = resolve(targetDir);
  mkdirSync(join(out, "src"), { recursive: true });
  mkdirSync(join(out, "bin"), { recursive: true });
  mkdirSync(join(out, "test"), { recursive: true });
  mkdirSync(join(out, "text", "text", "usascii"), { recursive: true });

  writeFileSync(join(out, "plugin.json"), genManifest(sp));
  writeFileSync(join(out, "src", `${cident(sp.id)}_cmds.h`), genCmdsH(sp));
  writeFileSync(join(out, "src", `${cident(sp.id)}_cmds.c`), genCmdsC(sp));
  writeFileSync(join(out, "src", `${cident(sp.id)}_adapter.c`), genAdapter(sp));
  writeFileSync(join(out, "src", `${cident(sp.id)}_main.c`), genMain(sp));
  writeFileSync(join(out, `${sp.id}.def`), genDef(sp));
  writeFileSync(join(out, "test", `${sp.id}_selftest.c`), genSelftest(sp));
  writeFileSync(join(out, "text", "text", "usascii", `${sp.id}_messages.txt`), genMessages(sp));
  writeFileSync(join(out, "README.md"), genBuildReadme(sp));
  if (!existsSync(join(out, "bin", `${sp.id}.dll`))) writeFileSync(join(out, "bin", `${sp.id}.dll`), "");  /* lint 占位 */

  console.log(`[plugin_init] generated six-module skeleton: ${out}`);
  console.log(`  id=${sp.id} commands=${1 + sp.cmds.length} (status + ${sp.cmds.length} stub${sp.cmds.length === 1 ? "" : "s"}) menu=${sp.menu ? "yes" : "no"}`);

  if (flag("--skip-lint")) { console.log("[plugin_init] lint skipped (--skip-lint)"); return; }

  /* 生成即验证：copy 流隔离根跑 manifest_lint */
  const lintRoot = join(ROOT, "test", "out", "plugin_init_lint");
  rmSync(lintRoot, { recursive: true, force: true });
  mkdirSync(join(lintRoot, sp.id), { recursive: true });
  cpSync(out, join(lintRoot, sp.id), { recursive: true });
  const r = spawnSync(process.execPath, [LINT, lintRoot], { stdio: "inherit" });
  if (r.status !== 0) { console.error("[plugin_init] LINT FAIL - fix the reported reasons and rerun"); process.exit(1); }
  console.log("[plugin_init] lint OK - next: fill business code, compile (see README.md), then the acceptance chain");
}

main().catch(e => { console.error("[plugin_init]", e.message); process.exit(1); });
