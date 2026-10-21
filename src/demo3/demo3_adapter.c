/* =====================================================================
 * creo-hub M3a: demo3 命令面适配器 —— L1 标准封装形态的活样例（docs\23 §6.1）
 * 本文件 = 插件仓库里唯一的 hub 相关代码：分发表 + 四个薄命令实现。
 * hub_cmd 导出体一行转发 hub_sdk_entry（双层错误纪律由 SDK 内建）。
 *
 * demo3.boom 的真实变更载体 = 棱柱导入特征，几何数据表驱动，数值逐字取自
 * PTC 官方样例 protk_appls\pt_userguide\ptu_featcreat\UgImportfeatCreate.c
 * （PTTestPrismCreate，x=12 y=10 z=8）；中间小对象按样例惯例不回收
 * （一次性实验调用，E11-10 内存断言只测 echo 循环，不受影响）。
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <ProToolkit.h>
#include <ProArray.h>
#include <ProMdl.h>
#include <ProModelitem.h>
#include <ProSolid.h>
#include <ProImportfeat.h>
#include <ProUtil.h>
#include <ProWchar.h>

#include "hub_sdk.h"
#include "demo3.h"

/* ---------------------------------------------------------- demo3.echo */

static cJSON* cmd_echo(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    const char* msg = hub_sdk_str(args, "msg");
    cJSON* r;
    (void)ctx;
    if (!msg) {
        ctx->err_code = "P3_ARGS";
        sprintf(hint, "msg (string) is required; example args {\"msg\":\"ping\"}");
        return NULL;
    }
    demo3_log("echo: %d byte(s)", (int)strlen(msg));
    r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "echo", msg);
    cJSON_AddNumberToObject(r, "bytes", (double)strlen(msg));
    return r;
}

/* ---------------------------------------------------------- demo3.slow */

static cJSON* cmd_slow(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    double sec = hub_sdk_num(args, "sec", -1.0);
    cJSON* r;
    if (!(sec > 0.0) || sec > 25.0) {
        ctx->err_code = "P3_ARGS";
        sprintf(hint, "sec (number, 0<sec<=25) required; E11-8 uses sec=5 (>timeoutSec 2)");
        return NULL;
    }
    demo3_log("slow: sleeping %.1fs on the Creo main thread (blocks pump!)", sec);
    Sleep((DWORD)(sec * 1000.0));
    demo3_log("slow: woke up");
    r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "slept_sec", sec);
    return r;
}

/* ------------------------------------------------------- demo3.longtask */

/* M4b async demo: N sleep steps on the main thread with SDK progress writes.
 * The hub answers 202 immediately and serves GET /jobs from the worker thread
 * while this runs (progress slot = sdk\hub_jobslot.h contract). */
static cJSON* cmd_longtask(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    int steps = (int)hub_sdk_num(args, "steps", 10);
    int ms = (int)hub_sdk_num(args, "ms", 200);
    int i;
    cJSON* r;
    if (steps < 1) steps = 1;
    if (steps > 50) steps = 50;
    if (ms < 0) ms = 0;
    if (ms > 1000) ms = 1000;
    demo3_log("longtask: %d steps x %dms (async; progress slot %s)", steps, ms,
              ctx->progress_map ? "present" : "absent");
    for (i = 0; i < steps; i++) {
        if (hub_sdk_time_left_ms(ctx) == 0) {
            sprintf(hint, "budget exhausted after %d/%d steps - async ETA too small "
                          "(raise timeoutSec in plugin.json)", i, steps);
            return NULL;
        }
        Sleep((DWORD)ms);
        hub_sdk_progress(ctx, (i + 1) * 100 / steps, "step");
    }
    r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "steps", steps);
    cJSON_AddNumberToObject(r, "ms", ms);
    cJSON_AddBoolToObject(r, "done", 1);
    return r;
}

/* ---------------------------------------------------------- demo3.fail */

static cJSON* cmd_fail(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    (void)args;
    ctx->err_code = "P3_FAIL";
    sprintf(hint, "deliberate business failure for E11-7: channel is healthy, "
                  "ok:false envelope passthrough is the expected result");
    demo3_log("fail: deliberate business failure returned");
    return NULL;
}

/* ---------------------------------------------------------- demo3.boom */

/* 棱柱几何表（样例 PTTestPrismCreate 数值固化：x=12, y=10, z=8）
 * M3d d1: P3_FACES Surface-2 行按官方样例逐字恢复（ISSUE-L2 缺面修复，
 * 诊断与论证 = docs\29 §2；此前该行 v3/origin/contour 三处转写偏差导致
 * 该面被 Creo 丢弃 -> 开壳 -> attempt_make_solid 降级为面片）。 */
typedef struct { int id; int s[2]; int dir[2]; double p1[3]; double p2[3]; } P3Edge;
static const P3Edge P3_EDGES[12] = {
    {  1, {1,2}, {0,1}, { 0, 0,0}, {12, 0, 0} },
    {  2, {1,5}, {0,1}, {12, 0,0}, {12,10, 0} },
    {  3, {1,4}, {0,1}, {12,10,0}, { 0,10, 0} },
    {  4, {1,3}, {0,1}, { 0,10,0}, { 0, 0, 0} },
    {  5, {6,2}, {1,0}, { 0, 0,8}, {12, 0, 8} },
    {  6, {6,5}, {1,0}, {12, 0,8}, {12,10, 8} },
    {  7, {6,4}, {1,0}, {12,10,8}, { 0,10, 8} },
    {  8, {6,3}, {1,0}, { 0,10,8}, { 0, 0, 8} },
    {  9, {2,3}, {0,1}, { 0, 0,0}, { 0, 0, 8} },
    { 10, {5,2}, {0,1}, {12, 0,0}, {12, 0, 8} },
    { 11, {4,5}, {0,1}, {12,10,0}, {12,10, 8} },
    { 12, {3,4}, {0,1}, { 0,10,0}, { 0,10, 8} }
};
typedef struct { int orient; double v1[3], v2[3], v3[3], o[3]; int edges[4]; } P3Face;
static const P3Face P3_FACES[6] = {
    { -1, { 1, 0,0}, { 0, 1,0}, { 0, 0, 1}, { 0, 0,0}, {1, 2, 3, 4} },
    {  1, { 1, 0,0}, { 0, 0,1}, { 0,-1, 0}, { 0, 0,0}, {1, 9, 5,10} },
    {  1, { 0,-1,0}, { 0, 0,1}, {-1, 0, 0}, { 0,10,0}, {4,12, 8, 9} },
    {  1, {-1, 0,0}, { 0, 0,1}, { 0, 1, 0}, {12,10,0}, {3,11, 7,12} },
    {  1, { 0, 1,0}, { 0, 0,1}, { 1, 0, 0}, {12, 0,0}, {2,10, 6,11} },
    {  1, { 1, 0,0}, { 0, 1,0}, { 0, 0, 1}, { 0, 0,8}, {5, 8, 7, 6} }
};

static ProIntfData* p3_prism_build(void)
{
    ProIntfData* intf = NULL;
    ProError st;
    int i;
    st = ProIntfDataAlloc(&intf);
    if (st != PRO_TK_NO_ERROR || !intf) return NULL;

    for (i = 0; i < 12; i++) {
        ProEdgedata* ed = NULL;
        ProCurvedata* cd = NULL;
        if (ProEdgedataAlloc(&ed) != PRO_TK_NO_ERROR) continue;
        if (ProCurvedataAlloc(&cd) != PRO_TK_NO_ERROR) continue;
        ProLinedataInit((double*)P3_EDGES[i].p1, (double*)P3_EDGES[i].p2, cd);
        ProEdgedataInit(P3_EDGES[i].id, (int*)P3_EDGES[i].s,
                        (ProEdgeDir*)P3_EDGES[i].dir, NULL, NULL, cd, ed);
        ProIntfDataEdgeAppend(intf, ed);
        ProCurvedataFree(cd);   /* 样例同款：curve 数据已并入 edge */
    }
    for (i = 0; i < 6; i++) {
        ProSurfaceshapedata sd;
        ProSurfacedata surf;
        ProContourdata* ct = NULL;
        ProContourdata* carr = NULL;
        int* eids = NULL;
        double uv0[2] = { 0.0, 0.0 };
        if (ProArrayAlloc(4, sizeof(int), 1, (ProArray*)&eids) != PRO_TK_NO_ERROR) continue;
        memcpy(eids, P3_FACES[i].edges, 4 * sizeof(int));
        if (ProArrayAlloc(1, sizeof(ProContourdata), 1, (ProArray*)&carr) != PRO_TK_NO_ERROR) continue;
        if (ProContourdataAlloc(&ct) != PRO_TK_NO_ERROR) continue;
        ProContourdataInit(1, ct);
        ProContourdataEdgeIdArraySet(ct, eids);
        carr[0] = *ct;
        ProPlanedataInit((double*)P3_FACES[i].v1, (double*)P3_FACES[i].v2,
                         (double*)P3_FACES[i].v3, (double*)P3_FACES[i].o, &sd);
        ProSurfacedataInit(PRO_SRF_PLANE, uv0, uv0,
                           (ProSurfaceOrient)P3_FACES[i].orient, &sd, i + 1, &surf);
        ProSurfacedataContourArraySet(&surf, carr);
        ProIntfDataSurfaceAppend(intf, &surf);
    }
    return intf;
}

static cJSON* cmd_boom(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    const char* name = hub_sdk_str(args, "name");
    ProSolid solid = NULL;
    ProMdl mdl = NULL;
    HubPartErr perr = HUB_PART_OK;
    ProError st;
    ProFeature feature;
    ProIntfDataSource ds;
    ProImportfeatAttr attr;
    ProIntfData* intf;
    wchar_t wname[64];
    cJSON* r;

    if (!name || !name[0] || strlen(name) > 60) {
        ctx->err_code = "P3_ARGS";
        sprintf(hint, "name (non-empty string, <=60 chars) required for the new feature");
        return NULL;
    }
    /* M3b R3: unified context resolution via SDK v1 helper (docs\24 §9) -
     * PART window direct, assembly window uses the ACTIVATED component,
     * anything else is an explicit self-healing error. */
    mdl = hub_sdk_active_part(&perr, hint);
    if (!mdl) {
        ctx->err_code = (perr == HUB_PART_NO_MODEL) ? "P3_NO_MODEL" :
                        (perr == HUB_PART_NO_ACTIVE_COMP) ? "P3_NO_ACTIVE_COMP" :
                        "P3_NOT_PART";
        demo3_log("boom: active_part failed err=%d hint=%s", (int)perr, hint);
        return NULL;
    }
    solid = (ProSolid)mdl;

    intf = p3_prism_build();
    if (!intf) {
        ctx->err_code = "P3_GEOM";
        sprintf(hint, "prism interface-data build failed (see demo3 log)");
        demo3_log("boom: ProIntfData build FAILED");
        return NULL;
    }
    st = ProIntfDataSourceInit(PRO_INTF_NEUTRAL, intf, &ds);
    if (st != PRO_TK_NO_ERROR) {
        ctx->err_code = "P3_DS_INIT";
        sprintf(hint, "ProIntfDataSourceInit failed ptk_status=%d", (int)st);
        demo3_log("boom: ProIntfDataSourceInit ret=%d", (int)st);
        return NULL;
    }
    attr.join_surfaces = PRO_B_TRUE;
    attr.attempt_make_solid = PRO_B_TRUE;
    attr.cut_or_add = PRO_B_FALSE;
    st = ProImportfeatCreate(solid, &ds, NULL, &attr, &feature);
    demo3_log("boom: ProImportfeatCreate ret=%d (name='%s')", (int)st, name);
    if (st != PRO_TK_NO_ERROR) {
        ctx->err_code = "P3_FEAT_CREATE";
        sprintf(hint, "ProImportfeatCreate failed ptk_status=%d - feature NOT created; "
                      "check active part is regen-clean", (int)st);
        return NULL;
    }
    ProStringToWstring(wname, (char*)name);
    st = ProModelitemNameSet(&feature, wname);
    demo3_log("boom: ProModelitemNameSet ret=%d", (int)st);

    r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", name);
    cJSON_AddBoolToObject(r, "created", 1);
    cJSON_AddNumberToObject(r, "rename_status", (double)st);
    return r;
}

/* ------------------------------------------------- hub_cmd 导出（一行） */

static const HubCmdEntry G_CMDS[] = {
    { "demo3.echo", cmd_echo },
    { "demo3.slow", cmd_slow },
    { "demo3.longtask", cmd_longtask },
    { "demo3.fail", cmd_fail },
    { "demo3.boom", cmd_boom },
    { NULL, NULL }
};

PRO_TK_DLL_EXPORT ProError hub_cmd(ProArgument* inputs, ProArgument** outputs)
{
    return hub_sdk_entry(inputs, outputs, G_CMDS);
}
