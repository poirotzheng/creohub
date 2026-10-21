/* =====================================================================
 * creo-hub M3a DEV-ONLY 实现：E11 通道实验端点（契约见 dev_e11.h）。
 *
 * 请求（POST /api/v1/_e11，令牌/JSON/审计走既有 post_op 全链）：
 *   { "route":"B"|"A",           缺省 "B"（进程内直调）；"A"=ProToolkitTaskExecute
 *     "plugin":"demo3",          缺省 "demo3"
 *     "func":"hub_cmd",          缺省 "hub_cmd"；E11-5 用 "no_such_fn"
 *     "in_json":"{...}",         IN_JSON 载荷文本（SDK 协议 {"command","args"}）
 *     "enc":"char"|"wchar",      缺省 char（PRO_VALUE_TYPE_STRING UTF-8）；
 *                                wchar=WSTRING（E11-3/4 编码对比）
 *     "mode":"direct"|"mem",     mem=只回进程内存（E11-10 采样，不触插件）
 *     "timeoutMs":N }            由 post_op 统一解析（信封层），本文件不读
 * 响应（200 常规；404 无此插件；409 非 ACTIVE=E11-6"状态校验先于通道"）：
 *   { ok, route, channel:"OK|MODULE_NULL|PROCADDR_NULL|HANDLE_NULL|TASK_<ERR>",
 *     function_return:"<ProError名>", out_json:"<OUT_JSON 文本>", enc, ms }
 *   mem 模式：{ ok:true, mode:"mem", mem_kb, peak_kb }
 * 通道纪律：inputs/outputs 均 ProArgumentProarrayFree 释放（E11-10 释放纪律
 * 的被测方之一）；每次调用 hub_log 落 [E11] 行（通道/ms/返回码全留痕）。
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psapi.h>

#include <ProToolkit.h>
#include <ProToolkitDll.h>
#include <ProArray.h>
#include <ProValue.h>
#include <ProWchar.h>

#include "dev_e11.h"
#include "hub_log.h"
#include "registry.h"
#include "adapter_ptk.h"   /* M3b b1: call primitives promoted to prod adapter; dev reuses same source */
#include "cJSON.h"

#define TAG "E11"

/* 响应装填（带容量防线：装不下=结构化错误，不截断半截 JSON） */
static int e11_respond(cJSON* root, char* out, int outsz)
{
    char* txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"response alloc failed\"}");
        out[outsz - 1] = '\0';
        return 200;
    }
    if ((int)strlen(txt) >= outsz) {
        cJSON_free(txt);
        _snprintf(out, outsz,
                  "{\"ok\":false,\"error\":\"result too large for envelope\","
                  "\"hint\":\"dev RESULT_CAP exceeded; shrink payload\"}");
        out[outsz - 1] = '\0';
        return 200;
    }
    strcpy(out, txt);
    cJSON_free(txt);
    return 200;
}

static cJSON* e11_base(int ok, const char* route, const char* channel)
{
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    if (route) cJSON_AddStringToObject(r, "route", route);
    if (channel) cJSON_AddStringToObject(r, "channel", channel);
    return r;
}

/* IN_JSON 装载：M3b b1 已升产 adapter_ptk.c（官方机制原样），dev 复用同源 */
static ProArgument* e11_build_inputs(const char* in_json, int as_wchar)
{ return adapter_build_inputs(in_json, as_wchar); }

/* OUT_JSON 提取：M3b b1 已升产 adapter_ptk.c（首见字符串值、label 命中优先），dev 复用同源 */
static char* e11_extract_out(ProArgument* outputs, int* n_out, char* label_seen, int lsz)
{ return adapter_extract_out_json(outputs, n_out, label_seen, lsz); }

/* ------------------------------------------------------------ 执行体 */

int dev_e11_execute(const char* payload, char* out, int outsz,
                    char* idout, char* stateout)
{
    cJSON* req;
    cJSON* r;
    const char* route = "B";
    const char* plugin = "demo3";
    const char* func = "hub_cmd";
    const char* in_json = "";
    const char* enc = "char";
    const char* mode = "direct";
    const cJSON* j;
    int idx;
    PluginRec* p;
    ProArgument* inputs = NULL;
    ProArgument* outputs = NULL;
    ProError fret = PRO_TK_NO_ERROR;
    ULONGLONG t0, ms;
    char* out_json = NULL;
    char label_seen[64];
    int n_out = 0;
    int as_wchar = 0;
    char funcbuf[64];

    strcpy(idout, "-");
    stateout[0] = '\0';

    req = cJSON_Parse(payload);
    if (!req) {
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad json\"}");
        out[outsz - 1] = '\0';
        strcpy(stateout, "bad-json");
        return 400;
    }
    j = cJSON_GetObjectItemCaseSensitive(req, "route");
    if (cJSON_IsString(j)) route = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "plugin");
    if (cJSON_IsString(j)) plugin = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "func");
    if (cJSON_IsString(j)) func = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "in_json");
    if (cJSON_IsString(j)) in_json = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "enc");
    if (cJSON_IsString(j)) enc = j->valuestring;
    j = cJSON_GetObjectItemCaseSensitive(req, "mode");
    if (cJSON_IsString(j)) mode = j->valuestring;
    as_wchar = (strcmp(enc, "wchar") == 0);
    strncpy(funcbuf, func, sizeof(funcbuf) - 1);
    funcbuf[sizeof(funcbuf) - 1] = '\0';

    /* ---- mem 采样（E11-10；不触插件、不触 PTK） ---- */
    if (strcmp(mode, "mem") == 0) {
        PROCESS_MEMORY_COUNTERS pmc;
        memset(&pmc, 0, sizeof(pmc));
        pmc.cb = sizeof(pmc);
        r = cJSON_CreateObject();
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
            cJSON_AddBoolToObject(r, "ok", 1);
            cJSON_AddStringToObject(r, "mode", "mem");
            cJSON_AddNumberToObject(r, "mem_kb", (double)(pmc.WorkingSetSize / 1024));
            cJSON_AddNumberToObject(r, "peak_kb", (double)(pmc.PeakWorkingSetSize / 1024));
        } else {
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "error", "GetProcessMemoryInfo failed");
        }
        cJSON_Delete(req);
        strcpy(stateout, "mem");
        return e11_respond(r, out, outsz);
    }

    /* ---- 状态门（E11-6：404/409 先于通道，与未来 /call 校验链同序） ---- */
    idx = registry_find_idx(plugin);
    if (idx < 0) {
        cJSON_Delete(req);
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"plugin not found\"}");
        out[outsz - 1] = '\0';
        _snprintf(stateout, 96, "plugin=%s missing", plugin);
        return 404;
    }
    p = &g_plugins[idx];
    strcpy(idout, p->id);
    if (p->state != PS_ACTIVE) {
        cJSON_Delete(req);
        r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 0);
        cJSON_AddStringToObject(r, "error", "plugin not active");
        cJSON_AddStringToObject(r, "state", registry_state_name(p->state));
        _snprintf(stateout, 96, "%s", registry_state_name(p->state));
        /* e11_respond 吃掉 r；但这里要 409 —— 手工装填 */
        {
            char* txt = cJSON_PrintUnformatted(r);
            cJSON_Delete(r);
            if (txt) { strncpy(out, txt, outsz - 1); out[outsz - 1] = '\0'; cJSON_free(txt); }
        }
        return 409;
    }

    /* ---- 通道 ---- */
    inputs = e11_build_inputs(in_json, as_wchar);
    if (!inputs) {
        cJSON_Delete(req);
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"input build failed (OOM)\"}");
        out[outsz - 1] = '\0';
        strcpy(stateout, "input-build-failed");
        return 200;
    }

    t0 = GetTickCount64();
    {
        /* M3b b1: 路线执行统一走 adapter_call（行为与 M3a 实测原语等价；
         * channel 字符串集合不变：OK/MODULE_NULL/PROCADDR_NULL/HANDLE_NULL/TASK_*） */
        AdapterChannelStatus chs;
        char chbuf[64];
        chs = adapter_call(p, route, funcbuf, inputs, &outputs, &fret, chbuf, sizeof(chbuf));
        ms = GetTickCount64() - t0;
        if (chs != ADP_CH_OK) {
            hub_log(TAG, "route=%s plugin=%s func=%s channel=%s function_return=%d(%s) ms=%llu",
                    route, p->id, funcbuf, chbuf, (int)fret, hub_errname(fret), ms);
            r = e11_base(0, route, chbuf);
            cJSON_AddStringToObject(r, "function_return", hub_errname(fret));
            cJSON_AddNumberToObject(r, "ms", (double)ms);
            goto done_req;
        }
        hub_log(TAG, "route=%s plugin=%s func=%s channel=OK function_return=%d(%s) ms=%llu",
                route, p->id, funcbuf, (int)fret, hub_errname(fret), ms);
        r = e11_base(1, route, "OK");
    }

    /* ---- OUT_JSON 提取（两条通道汇合） ---- */
    out_json = e11_extract_out(outputs, &n_out, label_seen, sizeof(label_seen));
    cJSON_AddStringToObject(r, "function_return", hub_errname(fret));
    cJSON_AddNumberToObject(r, "ms", (double)ms);
    cJSON_AddStringToObject(r, "enc", enc);
    cJSON_AddNumberToObject(r, "out_n", (double)n_out);
    if (label_seen[0]) cJSON_AddStringToObject(r, "out_label", label_seen);
    if (out_json) cJSON_AddStringToObject(r, "out_json", out_json);
    else cJSON_AddNullToObject(r, "out_json");

done_req:
    /* 释放纪律（E11-10 被测项）：outputs 无论谁分配都用官方 free；inputs 调用方所有 */
    if (outputs) ProArgumentProarrayFree(&outputs);
    if (inputs) ProArgumentProarrayFree(&inputs);
    if (out_json) free(out_json);
    cJSON_Delete(req);
    _snprintf(stateout, 96, "route=%s channel=%s ms=%llu",
              route,
              cJSON_GetObjectItemCaseSensitive(r, "channel") ?
                  cJSON_GetObjectItemCaseSensitive(r, "channel")->valuestring : "?",
              ms);
    return e11_respond(r, out, outsz);
}
