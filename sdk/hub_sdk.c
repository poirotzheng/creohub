/* =====================================================================
 * creo-hub 插件 SDK v1 实现（hub_sdk.h 契约；两文件即入，MIT，随包拷贝）
 * 依赖：cJSON（静态编入插件）+ PTK 头与库（ProArray 族、ProValuedata 族、
 * ProArgumentProarrayFree、ProMdl 上下文族）。
 * 离线可测：JSON 层与分发层不触 Creo；上述 PTK 函数可桩
 * （见 test 目录下 sdk_test.c，DoD D12）。
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <ProArray.h>
#include <ProValue.h>
#include <ProWchar.h>
#include <ProMdl.h>
#include <ProAssembly.h>

#include "hub_sdk.h"
#include "hub_jobslot.h"   /* v2: async 进度槽共享契约（SDK <-> hub） */

/* ------------------------------------------------------------ v1 审计钩 */

static HubSdkAuditFn g_sdk_audit = NULL;

void hub_sdk_set_audit(HubSdkAuditFn fn) { g_sdk_audit = fn; }

static void sdk_audit(const char* fmt, ...)
{
    char line[256];
    va_list ap;
    if (!g_sdk_audit) return;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    g_sdk_audit(line);
}

/* ------------------------------------------------------------ 小工具 */

static void sdk_slot_release(HubCmdCtx* ctx);   /* v2：入口返回前解除进度映射 */

static char* sdk_dup8(const char* s)
{
    size_t n;
    char* b;
    if (!s) return NULL;
    n = strlen(s) + 1;
    b = (char*)malloc(n);
    if (b) memcpy(b, s, n);
    return b;
}

static char* sdk_w_to_utf8(const wchar_t* w)
{
    int n;
    char* b;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    b = (char*)malloc((size_t)n);
    if (!b) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, b, n, NULL, NULL) <= 0) {
        free(b);
        return NULL;
    }
    return b;
}

#ifdef HUB_SDK_PAYLOAD_WCHAR
static wchar_t* sdk_utf8_to_w(const char* s)
{
    int n;
    wchar_t* b;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    b = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!b) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, b, n) <= 0) {
        free(b);
        return NULL;
    }
    return b;
}
#endif

/* IN_JSON 定位：label 精确命中优先；否则收第一个字符串值（宽容降级，
 * E11 实验期 label 绑定行为待实测——官方样例 ptu_featcreat 即手写扫描）。
 * 返回 malloc 的 UTF-8 文本（调用方 free），未找到返回 NULL。 */
static char* sdk_find_in_json(ProArgument* inputs)
{
    int size = 0, i;
    char* any = NULL;
    if (!inputs) return NULL;
    if (ProArraySizeGet((ProArray)inputs, &size) != PRO_TK_NO_ERROR) return NULL;
    for (i = 0; i < size; i++) {
        ProValueData* v = &inputs[i].value;
        char* txt = NULL;
        if (v->type == PRO_VALUE_TYPE_STRING && v->v.s)
            txt = sdk_dup8(v->v.s);
        else if (v->type == PRO_VALUE_TYPE_WSTRING && v->v.w)
            txt = sdk_w_to_utf8(v->v.w);
        if (!txt) continue;
        if (wcscmp(inputs[i].label, L"IN_JSON") == 0) {
            free(any);
            return txt;                 /* label 命中即定案 */
        }
        if (!any) any = txt;            /* 记住第一个字符串值兜底 */
        else free(txt);
    }
    return any;
}

/* OUT_JSON 装填（官方样例姿势：ProArrayAlloc(0,…)+ObjectAdd(-1 追加)；
 * 字符串经 ProValuedata*Set 存入 PTC 核心内存，调用方 ProArgumentProarrayFree）。*/
static ProError sdk_emit_out(ProArgument** outputs, const char* json_utf8)
{
    ProArgument arg;
    ProError st;
    if (!outputs || !json_utf8) return PRO_TK_BAD_INPUTS;
    st = ProArrayAlloc(0, sizeof(ProArgument), 1, (ProArray*)outputs);
    if (st != PRO_TK_NO_ERROR) return st;
    memset(&arg, 0, sizeof(arg));
    wcscpy(arg.label, L"OUT_JSON");
#ifdef HUB_SDK_PAYLOAD_WCHAR
    {
        wchar_t* w = sdk_utf8_to_w(json_utf8);
        if (!w) { ProArgumentProarrayFree(outputs); return PRO_TK_GENERAL_ERROR; }
        st = ProValuedataWstringSet(&arg.value, w);
        free(w);   /* Set 内部拷贝（官方机制注释）；桩与真机同语义 */
    }
#else
    st = ProValuedataStringSet(&arg.value, (char*)json_utf8);
#endif
    if (st != PRO_TK_NO_ERROR) { ProArgumentProarrayFree(outputs); return st; }
    st = ProArrayObjectAdd((ProArray*)outputs, -1, 1, &arg);
    if (st != PRO_TK_NO_ERROR) { ProArgumentProarrayFree(outputs); return st; }
    return PRO_TK_NO_ERROR;
}

static cJSON* sdk_mk_err(const char* code, const char* msg, const char* hint)
{
    cJSON* o = cJSON_CreateObject();
    if (!o) return NULL;
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "err_code", code ? code : "SDK_ERROR");
    cJSON_AddStringToObject(o, "err_msg", msg ? msg : "");
    cJSON_AddStringToObject(o, "hint", hint ? hint : "");
    return o;
}

/* ------------------------------------------------------------ 主入口 */

ProError hub_sdk_entry(ProArgument* inputs, ProArgument** outputs,
                       const HubCmdEntry* table)
{
    static const char* OOM_JSON =
        "{\"ok\":false,\"err_code\":\"SDK_OOM\","
        "\"err_msg\":\"response allocation failed\",\"hint\":\"retry or see hub log\"}";
    char* in_text = NULL;
    cJSON* root = NULL;
    cJSON* jc = NULL;
    cJSON* ja = NULL;
    cJSON* ja_mine = NULL;      /* args 缺失时 SDK 造的空对象（需单独释放） */
    cJSON* out_root = NULL;
    cJSON* result = NULL;
    char* out_text = NULL;
    char hint[HUB_SDK_HINT_SZ];
    HubCmdCtx ctx;
    const HubCmdEntry* e;
    ULONGLONG t0 = GetTickCount64();
    ProError st;

    hint[0] = '\0';
    ctx.sdk_version = HUB_SDK_VERSION;
    ctx.timeout_budget_ms = 0;
    ctx.err_code = "PLUGIN_ERROR";
    ctx.start_tick = (unsigned long long)t0;
    ctx.progress_map = NULL;      /* v2 */
    ctx._progress_view = NULL;    /* v2 */

    if (!outputs) return PRO_TK_BAD_INPUTS;
    *outputs = NULL;
    if (!table) {
        out_root = sdk_mk_err("SDK_BAD_TABLE", "dispatch table missing",
                              "plugin build error: pass a NULL-terminated HubCmdEntry table");
        goto emit;
    }

    in_text = sdk_find_in_json(inputs);
    if (!in_text) {
        out_root = sdk_mk_err("SDK_BAD_INPUT",
                              "IN_JSON argument missing or not a string value",
                              "hub sent no IN_JSON payload; check the call channel/route");
        goto emit;
    }
    root = cJSON_Parse(in_text);
    if (!root) {
        out_root = sdk_mk_err("SDK_BAD_JSON", "IN_JSON is not valid JSON",
                              "check payload serialization on the hub side");
        goto emit;
    }
    jc = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(jc) || !jc->valuestring[0]) {
        out_root = sdk_mk_err("SDK_NO_COMMAND",
                              "IN_JSON has no string field 'command'",
                              "payload must be {\"command\":\"<id>.<cmd>\",\"args\":{...}}");
        goto emit;
    }
    ja = cJSON_GetObjectItemCaseSensitive(root, "args");
    if (!ja) { ja = cJSON_CreateObject(); ja_mine = ja; }   /* 宽容：缺 args=空对象 */

    /* v1: hub 注入的预算上下文（可选；v0 hub 无 _ctx -> budget 0=未知） */
    {
        cJSON* jctx = cJSON_GetObjectItemCaseSensitive(root, "_ctx");
        if (cJSON_IsObject(jctx)) {
            cJSON* jb = cJSON_GetObjectItemCaseSensitive(jctx, "budget_ms");
            cJSON* jp = cJSON_GetObjectItemCaseSensitive(jctx, "progress_map");
            if (cJSON_IsNumber(jb) && jb->valueint > 0)
                ctx.timeout_budget_ms = jb->valueint;
            /* v2: async job 进度槽名（可选；同步命令/旧 hub 无此字段） */
            if (cJSON_IsString(jp) && jp->valuestring && jp->valuestring[0])
                ctx.progress_map = jp->valuestring;
        }
    }

    e = NULL;
    {
        const HubCmdEntry* it;
        for (it = table; it->cmd_id; it++) {
            if (strcmp(it->cmd_id, jc->valuestring) == 0) { e = it; break; }
        }
    }
    if (!e || !e->fn) {
        sdk_audit("dispatch MISS cmd=%s", jc->valuestring);
        out_root = sdk_mk_err("CMD_NOT_FOUND", jc->valuestring,
                              "command id not in this plugin's dispatch table; "
                              "check plugin.json commands[] vs hub_cmd table");
        goto emit;
    }
    sdk_audit("dispatch HIT cmd=%s budget_ms=%d", jc->valuestring, ctx.timeout_budget_ms);

    result = e->fn(ja, &ctx, hint);
    hint[HUB_SDK_HINT_SZ - 1] = '\0';
    if (!result) {
        /* 业务失败：err_code 由命令函数经 ctx 提供（缺省 PLUGIN_ERROR） */
        sdk_audit("biz-fail cmd=%s err_code=%s", jc->valuestring,
                  ctx.err_code ? ctx.err_code : "PLUGIN_ERROR");
        out_root = sdk_mk_err(ctx.err_code ? ctx.err_code : "PLUGIN_ERROR",
                              hint[0] ? hint : "command reported failure",
                              hint);
    } else {
        out_root = cJSON_CreateObject();
        if (out_root) {
            cJSON_AddBoolToObject(out_root, "ok", 1);
            cJSON_AddItemToObject(out_root, "result", result);
            result = NULL;   /* 所有权移交给 out_root */
            cJSON_AddNumberToObject(out_root, "ms",
                                    (double)(GetTickCount64() - t0));
            sdk_audit("ok cmd=%s ms=%llu", jc->valuestring,
                      (unsigned long long)(GetTickCount64() - t0));
        }
    }

emit:
    sdk_slot_release(&ctx);        /* v2: 解除进度映射（无槽=no-op） */
    out_text = out_root ? cJSON_PrintUnformatted(out_root) : NULL;
    if (out_root) cJSON_Delete(out_root);
    if (result) cJSON_Delete(result);
    if (ja_mine) cJSON_Delete(ja_mine);
    if (root) cJSON_Delete(root);
    if (in_text) free(in_text);

    if (!out_text) {
        /* 最后防线：固定错误 JSON（无分配路径），通道层仍尽力送出去 */
        st = sdk_emit_out(outputs, OOM_JSON);
        return (st == PRO_TK_NO_ERROR) ? PRO_TK_NO_ERROR : PRO_TK_GENERAL_ERROR;
    }
    st = sdk_emit_out(outputs, out_text);
    cJSON_free(out_text);
    return (st == PRO_TK_NO_ERROR) ? PRO_TK_NO_ERROR : PRO_TK_GENERAL_ERROR;
}

/* ------------------------------------------------------------ 取值助手 */

const char* hub_sdk_str(const cJSON* args, const char* key)
{
    const cJSON* it = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

double hub_sdk_num(const cJSON* args, const char* key, double dflt)
{
    const cJSON* it = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    return cJSON_IsNumber(it) ? it->valuedouble : dflt;
}

int hub_sdk_bool(const cJSON* args, const char* key, int dflt)
{
    const cJSON* it = args ? cJSON_GetObjectItemCaseSensitive(args, key) : NULL;
    if (cJSON_IsBool(it)) return cJSON_IsTrue(it) ? 1 : 0;
    return dflt;
}

/* ------------------------------------------------------------ v1 助手 */

long long hub_sdk_time_left_ms(const HubCmdCtx* ctx)
{
    unsigned long long elapsed;
    if (!ctx || ctx->timeout_budget_ms <= 0) return -1;   /* 未知/无预算 */
    elapsed = GetTickCount64() - ctx->start_tick;
    if ((long long)elapsed >= ctx->timeout_budget_ms) return 0;
    return (long long)(ctx->timeout_budget_ms - (int)elapsed);
}

/* ------------------------------------------------- v2 async 进度槽写入器 */

/* 单写者 seq 协议（sdk\hub_jobslot.h）：奇=写入中，偶=稳定。
 * 只在 pct/msg/state 之间递增两次，读完 seq 为偶即一致。 */
static void sdk_slot_publish(HubJobSlot* s, int state, int pct, const char* msg)
{
    size_t n;
    s->seq++;                                /* odd: 写入中 */
    s->state = state;
    s->pct = pct;
    s->tick_ms = (unsigned long)(GetTickCount64() & 0xFFFFFFFFu);
    s->msg[0] = '\0';
    if (msg && msg[0]) {
        n = strlen(msg);
        if (n > sizeof(s->msg) - 1) n = sizeof(s->msg) - 1;
        memcpy(s->msg, msg, n);
        s->msg[n] = '\0';
    }
    s->seq++;                                /* even: 稳定 */
}

int hub_sdk_progress(HubCmdCtx* ctx, int pct, const char* msg)
{
    HubJobSlot* s;
    if (!ctx || !ctx->progress_map || !ctx->progress_map[0]) return 0;
    if (!ctx->_progress_view) {
        HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, ctx->progress_map);
        void* v;
        if (!h) return 0;                                  /* 无槽 -> no-op */
        v = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HubJobSlot));
        CloseHandle(h);
        if (!v) return 0;
        ctx->_progress_view = v;
    }
    s = (HubJobSlot*)ctx->_progress_view;
    if (s->magic != HUB_JOBSLOT_MAGIC) return 0;           /* 异版本槽 -> no-op */
    if (pct < HUB_JOBSLOT_PCT_UNKNOWN) pct = HUB_JOBSLOT_PCT_UNKNOWN;
    if (pct > 100) pct = 100;
    sdk_slot_publish(s, HUB_JOBSLOT_ST_WORKING, pct, msg);
    return 1;
}

/* 入口返回前解除映射（幂等；无槽时 no-op） */
static void sdk_slot_release(HubCmdCtx* ctx)
{
    if (ctx && ctx->_progress_view) {
        UnmapViewOfFile(ctx->_progress_view);
        ctx->_progress_view = NULL;
    }
}

ProMdl hub_sdk_active_part(HubPartErr* err, char err_hint[HUB_SDK_HINT_SZ])
{
    ProMdl cur = NULL, act = NULL;
    ProMdlType t;
    ProError st;
    HubPartErr e = HUB_PART_OK;

    if (err_hint) err_hint[0] = '\0';

    st = ProMdlCurrentGet(&cur);
    if (st != PRO_TK_NO_ERROR || !cur) {
        e = HUB_PART_NO_MODEL;
        if (err_hint)
            _snprintf(err_hint, HUB_SDK_HINT_SZ,
                      "no current model window - open or retrieve a part first (PTK %d)", (int)st);
        if (err) *err = e;
        return NULL;
    }
    if (ProMdlTypeGet(cur, &t) != PRO_TK_NO_ERROR) {
        e = HUB_PART_PTK_ERR;
        if (err_hint)
            _snprintf(err_hint, HUB_SDK_HINT_SZ, "ProMdlTypeGet failed on current model");
        if (err) *err = e;
        return NULL;
    }
    if (t == PRO_PART) {
        if (err) *err = HUB_PART_OK;
        return cur;
    }
    if (t == PRO_ASSEMBLY) {
        st = ProMdlActiveGet(&act);
        if (st != PRO_TK_NO_ERROR) {
            e = HUB_PART_PTK_ERR;
            if (err_hint)
                _snprintf(err_hint, HUB_SDK_HINT_SZ, "ProMdlActiveGet failed (PTK %d)", (int)st);
            if (err) *err = e;
            return NULL;
        }
        if (!act || act == cur) {
            e = HUB_PART_NO_ACTIVE_COMP;
            if (err_hint)
                _snprintf(err_hint, HUB_SDK_HINT_SZ,
                          "assembly window with no activated component - "
                          "activate a part in the model tree, then retry");
            if (err) *err = e;
            return NULL;
        }
        if (ProMdlTypeGet(act, &t) == PRO_TK_NO_ERROR && t == PRO_PART) {
            if (err) *err = HUB_PART_OK;
            return act;
        }
        e = HUB_PART_NOT_PART;
        if (err_hint)
            _snprintf(err_hint, HUB_SDK_HINT_SZ,
                      "activated object is not a PART (sub-assembly?) - activate a part component");
        if (err) *err = e;
        return NULL;
    }
    e = HUB_PART_NOT_PART;
    if (err_hint)
        _snprintf(err_hint, HUB_SDK_HINT_SZ,
                  "current model is neither PART nor ASSEMBLY (drawing/other)");
    if (err) *err = e;
    return NULL;
}

/* ------------------------------------------------------------ R2 ledger */

typedef struct { void* handle; char kind[24]; } SdkArmed;
static SdkArmed g_armed[HUB_SDK_MAX_ARMED];

void hub_sdk_arm(void* handle, const char* kind)
{
    int i, slot = -1;
    if (!handle) return;
    for (i = 0; i < HUB_SDK_MAX_ARMED; i++) {
        if (g_armed[i].handle == handle) { slot = i; break; }   /* re-arm refreshes */
        if (slot < 0 && !g_armed[i].handle) slot = i;
    }
    if (slot < 0) {
        sdk_audit("R2 ledger FULL - arm(%s) NOT tracked", kind ? kind : "?");
        return;
    }
    g_armed[slot].handle = handle;
    strncpy(g_armed[slot].kind, kind ? kind : "?", sizeof(g_armed[slot].kind) - 1);
    g_armed[slot].kind[sizeof(g_armed[slot].kind) - 1] = '\0';
}

void hub_sdk_disarm(void* handle)
{
    int i;
    for (i = 0; i < HUB_SDK_MAX_ARMED; i++)
        if (g_armed[i].handle == handle) {
            g_armed[i].handle = NULL;
            g_armed[i].kind[0] = '\0';
        }
}

int hub_sdk_armed_count(void)
{
    int i, n = 0;
    for (i = 0; i < HUB_SDK_MAX_ARMED; i++) if (g_armed[i].handle) n++;
    return n;
}

int hub_sdk_terminate_check(char* report, int reportsz)
{
    int i, n = 0, off = 0;
    if (report && reportsz > 0) report[0] = '\0';
    for (i = 0; i < HUB_SDK_MAX_ARMED; i++) {
        if (!g_armed[i].handle) continue;
        n++;
        if (report && off < reportsz - 1) {
            int w = _snprintf(report + off, reportsz - off, "%s%s",
                              n > 1 ? "," : "", g_armed[i].kind);
            if (w < 0 || w >= reportsz - off) break;
            off += w;
        }
    }
    if (report && reportsz > 0) report[reportsz - 1] = '\0';
    if (n > 0)
        sdk_audit("R2 terminate: %d callback(s) STILL ARMED: %s", n,
                  report ? report : "?");
    else
        sdk_audit("R2 terminate: clean (0 armed callbacks)");
    return n;
}
