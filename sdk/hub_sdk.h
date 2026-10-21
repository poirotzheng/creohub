/* =====================================================================
 * creo-hub 插件 SDK v1（M3b b1 定稿；docs\23 §5.2 / docs\24 §9 方案1）
 *
 * 两文件即入式 SDK：把 hub_sdk.h + hub_sdk.c 拷进插件工程（并把
 * cJSON.c/h 一并编入，见 src\hub\vendor\cjson），hub_cmd 导出的全部
 * PTK 通道样板由 SDK 承担。插件作者只写三样：
 *   1) 命令函数   cJSON* fn(const cJSON* args, HubCmdCtx* ctx, char* hint)
 *   2) 分发表     HubCmdEntry g_cmds[] = {{"<id>.<cmd>", fn},…,{NULL,NULL}}
 *   3) 一行导出   PRO_TK_DLL_EXPORT ProError hub_cmd(ProArgument* in,
 *                    ProArgument** out){ return hub_sdk_entry(in,out,g_cmds); }
 *
 * 双层错误纪律（SDK 内建，插件不可能写错层）：
 *   通道层  hub_cmd 恒返 PRO_TK_NO_ERROR；唯一例外=输出根本无法分配
 *           （返 PRO_TK_GENERAL_ERROR，调用方能看到通道级失败）。
 *   业务层  成败一律走 OUT_JSON：{ok:true,result:{…},ms:N} 或
 *           {ok:false,err_code:"…",err_msg:"…",hint:"…"}.
 *
 * 载荷协议（docs\22 §4.3，E11 实测定案 char* UTF-8 主形态）：
 *   IN_JSON  = 一个 ProArgument，label L"IN_JSON"，值 STRING(char* UTF-8)
 *              或 WSTRING(wchar_t*)——两种都收（编码对比零改插件）；
 *              文本 = {"command":"<id>.<cmd>","args":{…},"_ctx":{…}}
 *              （_ctx=hub v1 注入的预算上下文，SDK 消费、命令函数不碰）
 *   OUT_JSON = 一个 ProArgument，label L"OUT_JSON"，缺省 STRING(char*
 *              UTF-8)；编译期定义 HUB_SDK_PAYLOAD_WCHAR 切 WSTRING。
 *
 * 内存纪律（ProTkdllFunction 契约）：inputs 属于调用方，SDK 不释放；
 * outputs 由 SDK 经 ProArrayAlloc/ProValuedata*Set 分配，调用方用
 * ProArgumentProarrayFree 释放（官方样例 ptu_featcreat 同款姿势）。
 *
 * v1 变更（M3b b1，对 v0 完全向后兼容：无 _ctx 时行为同 v0）：
 *   - ctx.timeout_budget_ms 从 hub _ctx.budget_ms 实填（0=未知/旧 hub）；
 *   - 协作让路助手 hub_sdk_time_left_ms（主线程同步调用不可抢占——
 *     长命令在安全点自查预算并主动收尾，docs\22 §4.3 诚实语义）；
 *   - 可选审计钩 hub_sdk_set_audit（插件本地诊断出口，不落敏感内容）；
 *   - hub_sdk_active_part()：统一零件上下文解析（docs\24 §9 方案1）——
 *     PART 窗直取 current；装配窗取激活组件（须为 PART）；无激活组件
 *     或非零件 → 显式错误+模型可自愈 hint。
 *
 * v2 变更（M4b async 进度，docs\39 §3.3/§4.1；对 v1 源码完全兼容）：
 *   - ctx.progress_map：hub 每 job 建的命名共享内存名（同步命令=NULL）；
 *   - hub_sdk_progress(ctx,pct,msg)：向进度槽写一条进度（无槽=no-op，
 *     绝不影响业务结果）；契约头见 sdk\hub_jobslot.h；
 *   - HUB_SDK_VERSION 1->2（语义化：加性变更）。
 *   ⚠ 诚实边界：进度可见≠并行；主线程仍被本命令占用（docs\39 §3.1）。
 * ===================================================================== */
#ifndef HUB_SDK_H
#define HUB_SDK_H

#include <ProToolkit.h>
#include <ProToolkitDll.h>   /* ProArgument / ProTkdllFunction / PRO_TK_DLL_EXPORT */
#include <ProMdl.h>          /* v1: ProMdl / ProMdlTypeGet (active_part) */
#include "cJSON.h"

#define HUB_SDK_VERSION 2
#define HUB_SDK_HINT_SZ 256

#ifdef __cplusplus
extern "C" {
#endif

/* 传给命令函数的执行上下文（v1；v2 末尾追加进度槽字段——只增不破） */
typedef struct HubCmdCtx {
    int         sdk_version;       /* = HUB_SDK_VERSION */
    int         timeout_budget_ms; /* v1: hub _ctx.budget_ms（0=未知/旧 hub） */
    const char* err_code;          /* 失败时命令函数可改写（缺省 PLUGIN_ERROR） */
    unsigned long long start_tick; /* v1 内部：入口 GetTickCount64（助手消费） */
    /* ---- v2（M4b async 进度；v1 源码不读，零影响）---- */
    const char* progress_map;      /* hub _ctx.progress_map；NULL=非 async/旧 hub */
    void*       _progress_view;    /* SDK 内部：映射视图（勿读写；入口返回时自动解除） */
} HubCmdCtx;

/* 命令函数：成功=返回 result JSON（SDK 包 {ok:true,result:…}）；
 * 失败=填 err_hint（人类可读+模型可自愈）、可改 ctx->err_code，返回 NULL。
 * args 恒为 JSON object（请求缺 args 时=空对象），只读。 */
typedef cJSON* (*HubCmdFn)(const cJSON* args, HubCmdCtx* ctx,
                           char err_hint[HUB_SDK_HINT_SZ]);

/* 分发表项；末项 cmd_id=NULL 收口 */
typedef struct { const char* cmd_id; HubCmdFn fn; } HubCmdEntry;

/* ProTkdllFunction 兼容主入口（hub_cmd 导出体一行转发到这里） */
ProError hub_sdk_entry(ProArgument* inputs, ProArgument** outputs,
                       const HubCmdEntry* table);

/* 命令实现便捷取值（args 内查键；缺失/类型不符返回 NULL/缺省值） */
const char* hub_sdk_str(const cJSON* args, const char* key);
double      hub_sdk_num(const cJSON* args, const char* key, double dflt);
int         hub_sdk_bool(const cJSON* args, const char* key, int dflt);

/* v1 协作预算：剩余毫秒；-1=无预算（旧 hub/未知）。长命令在安全点自查，
 * 归零前主动收尾返回部分结果或诚实失败——主线程不可抢占的唯一正解。 */
long long   hub_sdk_time_left_ms(const HubCmdCtx* ctx);

/* v2 async 进度（M4b，docs\39 §3.3/§4.1）：
 *   向 hub 的 job 进度槽写一条进度（仅 async 命令有意义）。
 *   pct：0..100；-1=不确定（进度条走不确定态）；越界自动夹取。
 *   msg：UTF-8 短消息（可 NULL）；>191 字节截断。
 *   返回：1=已写入；0=无槽（同步命令/旧 hub/映射不可用）——**永远 no-op 安全**，
 *   绝不因进度失败影响业务结果。调用位置：循环安全点，与 time_left 组合：
 *     if (hub_sdk_time_left_ms(ctx) == 0) { ...诚实收尾... }
 *     hub_sdk_progress(ctx, done * 100 / total, "step ...");
 *   ⚠ 诚实边界：进度可见≠任务可并行；主线程仍被本命令占用（docs\39 §3.1）。 */
int         hub_sdk_progress(HubCmdCtx* ctx, int pct, const char* msg);

/* v1 可选审计钩（插件本地；NULL=关闭）。SDK 送短诊断行（ASCII，无载荷
 * 内容、无敏感信息）：分发命中/未命中、业务失败码、耗时。 */
typedef void (*HubSdkAuditFn)(const char* line);
void        hub_sdk_set_audit(HubSdkAuditFn fn);

/* v1 统一零件上下文解析（docs\24 §9 方案1；E12/M3a boom 实测教训）：
 *   PART 窗        -> current model 直取
 *   装配窗+激活零件 -> ProMdlActiveGet（激活组件）
 *   装配窗无激活    -> HUB_PART_NO_ACTIVE_COMP + hint（先激活组件）
 *   其他           -> 显式错误。
 * 成功返回 ProMdl（会话所有，勿释放）；失败返回 NULL、*err/*err_hint 填因。 */
typedef enum {
    HUB_PART_OK = 0,
    HUB_PART_NO_MODEL,        /* 无当前模型窗口 */
    HUB_PART_NOT_PART,        /* 解析对象不是 PART（装配中装配/工程图…） */
    HUB_PART_NO_ACTIVE_COMP,  /* 装配窗、无激活组件 */
    HUB_PART_PTK_ERR          /* 底层 PTK 调用失败（hint 带码） */
} HubPartErr;

ProMdl hub_sdk_active_part(HubPartErr* err, char err_hint[HUB_SDK_HINT_SZ]);

/* v1 (M3b R2) armed-callback ledger - plugin self-discipline instrument:
 * every PTK callback a plugin arms (ProUITimer / notifications / UI hooks)
 * SHOULD be registered here; user_terminate then calls
 * hub_sdk_terminate_check() and the report goes to the audit hook ("I still
 * hold N callbacks"). Registration is obligatory discipline - the hub cannot
 * see direct-armed callbacks (E12 lesson); the P2 hub-side ledger (b3) will
 * reconcile the self-report at unmount time. */
#define HUB_SDK_MAX_ARMED 16
void hub_sdk_arm(void* handle, const char* kind);
void hub_sdk_disarm(void* handle);
int  hub_sdk_armed_count(void);
/* returns armed count; when >0 fills report with "kind,kind,..."; also feeds
 * the audit hook so the line lands in the plugin's own log. */
int  hub_sdk_terminate_check(char* report, int reportsz);

#ifdef __cplusplus
}   /* extern "C" */
#endif

#endif /* HUB_SDK_H */
