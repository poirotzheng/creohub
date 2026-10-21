/* =====================================================================
 * creo-hub M3a: hub_sdk v0 offline test (DoD D12, docs\23 §8.1)
 * Stubs the five PTK allocator functions hub_sdk.c touches (LIB_*_API is
 * empty without DLL_IMPL -> plain extern, stub-linkable without PTK libs,
 * same pattern as test\registry_test.c + test_stubs.c).
 * Exit code: 0 = ALL PASS, 1 = failures.
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ProToolkit.h>
#include <ProArray.h>
#include <ProValue.h>
#include <ProToolkitDll.h>
#include <ProWchar.h>

#include "hub_sdk.h"
#include "hub_jobslot.h"   /* v2: 进度槽共享契约 */

/* -------------------------------------------------- PTK stubs (malloc) */

typedef struct { int size; int obj_size; int alloc; } StubHdr;
#define STUB_HDR(a) ((StubHdr*)((char*)(a) - sizeof(StubHdr)))

static char* stub_dup8(const char* s)
{
    size_t n = strlen(s) + 1; char* b = (char*)malloc(n);
    if (b) memcpy(b, s, n);
    return b;
}
static wchar_t* stub_dupw(const wchar_t* s)
{
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t); wchar_t* b = (wchar_t*)malloc(n);
    if (b) memcpy(b, s, n);
    return b;
}

ProError ProArrayAlloc(int n_objs, int obj_size, int reallocation_size,
                       ProArray* p_array)
{
    int cap = n_objs > 0 ? n_objs : (reallocation_size > 0 ? reallocation_size : 1);
    char* m = (char*)malloc(sizeof(StubHdr) + (size_t)cap * obj_size);
    StubHdr* h;
    if (!m || !p_array) return PRO_TK_OUT_OF_MEMORY;
    h = (StubHdr*)m;
    h->size = n_objs; h->obj_size = obj_size; h->alloc = cap;
    memset(m + sizeof(StubHdr), 0, (size_t)cap * obj_size);
    *p_array = (ProArray)(m + sizeof(StubHdr));
    return PRO_TK_NO_ERROR;
}

ProError ProArraySizeGet(ProArray array, int* p_size)
{
    if (!array || !p_size) return PRO_TK_BAD_INPUTS;
    *p_size = STUB_HDR(array)->size;
    return PRO_TK_NO_ERROR;
}

ProError ProArrayObjectAdd(ProArray* p_array, int index, int n_objects,
                           void* p_object)
{
    StubHdr* h;
    char* base;
    if (!p_array || !*p_array || !p_object || n_objects != 1)
        return PRO_TK_BAD_INPUTS;
    h = STUB_HDR(*p_array);
    if (index < 0) index = h->size;              /* PRO_VALUE_UNUSED = append */
    if (index != h->size) return PRO_TK_BAD_INPUTS;  /* test stub: append-only */
    if (h->size + 1 > h->alloc) {
        int ncap = h->alloc * 2 + 1;
        char* m = (char*)realloc((char*)h, sizeof(StubHdr) + (size_t)ncap * h->obj_size);
        if (!m) return PRO_TK_OUT_OF_MEMORY;
        h = (StubHdr*)m;
        h->alloc = ncap;
        *p_array = (ProArray)(m + sizeof(StubHdr));
    }
    base = (char*)(*p_array) + (size_t)h->size * h->obj_size;
    memcpy(base, p_object, (size_t)h->obj_size);
    h->size++;
    return PRO_TK_NO_ERROR;
}

ProError ProArrayFree(ProArray* p_array)
{
    if (!p_array || !*p_array) return PRO_TK_BAD_INPUTS;
    free((char*)(*p_array) - sizeof(StubHdr));
    *p_array = NULL;
    return PRO_TK_NO_ERROR;
}

ProError ProValuedataStringSet(ProValueData* value, char* string)
{
    if (!value || !string) return PRO_TK_GENERAL_ERROR;
    value->type = PRO_VALUE_TYPE_STRING;
    value->v.s = stub_dup8(string);
    return value->v.s ? PRO_TK_NO_ERROR : PRO_TK_GENERAL_ERROR;
}

ProError ProValuedataWstringSet(ProValueData* value, wchar_t* wstring)
{
    if (!value || !wstring) return PRO_TK_GENERAL_ERROR;
    value->type = PRO_VALUE_TYPE_WSTRING;
    value->v.w = stub_dupw(wstring);
    return value->v.w ? PRO_TK_NO_ERROR : PRO_TK_GENERAL_ERROR;
}

ProError ProArgumentProarrayFree(ProArgument** array)
{
    int i, n = 0;
    if (!array || !*array) return PRO_TK_BAD_INPUTS;
    ProArraySizeGet((ProArray)*array, &n);
    for (i = 0; i < n; i++) {
        ProValueData* v = &(*array)[i].value;
        if (v->type == PRO_VALUE_TYPE_STRING && v->v.s) { free(v->v.s); v->v.s = NULL; }
        if (v->type == PRO_VALUE_TYPE_WSTRING && v->v.w) { free(v->v.w); v->v.w = NULL; }
    }
    return ProArrayFree((ProArray*)array);
}

/* M3b b1: hub_sdk v1 active_part references (offline stubs: "no model") */
ProError ProMdlCurrentGet(ProMdl* p_handle)
{
    (void)p_handle;
    return PRO_TK_GENERAL_ERROR;
}
ProError ProMdlNameGet(ProMdl handle, ProName name)
{
    (void)handle;
    name[0] = L'\0';
    return PRO_TK_NO_ERROR;
}
ProError ProMdlTypeGet(ProMdl model, ProMdlType* p_type)
{
    (void)model;
    *p_type = PRO_PART;
    return PRO_TK_NO_ERROR;
}
ProError ProMdlActiveGet(ProMdl* p_handle)
{
    (void)p_handle;
    return PRO_TK_GENERAL_ERROR;
}

/* ------------------------------------------------------ test scaffolding */

static int g_pass = 0, g_fail = 0;
static void CHECK(int cond, const char* what)
{
    if (cond) { g_pass++; printf("  [PASS] %s\n", what); }
    else      { g_fail++; printf("  [FAIL] %s\n", what); }
}

/* build a one-element IN_JSON input array like the hub does */
static ProArgument* mk_inputs(const wchar_t* label, const char* utf8, int as_wstring)
{
    ProArgument* in = NULL;
    ProArgument arg;
    ProArrayAlloc(0, sizeof(ProArgument), 1, (ProArray*)&in);
    memset(&arg, 0, sizeof(arg));
    wcscpy(arg.label, label);
    if (as_wstring) {
        int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
        wchar_t* w = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n);
        ProValuedataWstringSet(&arg.value, w);
        free(w);
    } else {
        ProValuedataStringSet(&arg.value, (char*)utf8);
    }
    ProArrayObjectAdd((ProArray*)&in, -1, 1, &arg);
    return in;
}

/* run one call; returns parsed OUT_JSON (caller cJSON_Delete); *ret = ProError */
static cJSON* run(const char* in_json, int as_wstring, const HubCmdEntry* tbl,
                  ProError* ret, int* out_n)
{
    ProArgument* in = mk_inputs(L"IN_JSON", in_json, as_wstring);
    ProArgument* out = NULL;
    cJSON* parsed = NULL;
    *ret = hub_sdk_entry(in, &out, tbl);
    if (out) {
        ProArraySizeGet((ProArray)out, out_n);
        if (*out_n >= 1 && out[0].value.type == PRO_VALUE_TYPE_STRING && out[0].value.v.s)
            parsed = cJSON_Parse(out[0].value.v.s);
        CHECK(*out_n >= 1 && wcscmp(out[0].label, L"OUT_JSON") == 0,
              "  (outputs: 1 element labeled OUT_JSON)");
        ProArgumentProarrayFree(&out);
    } else {
        *out_n = 0;
    }
    ProArgumentProarrayFree(&in);
    return parsed;
}

static const char* jstr(cJSON* o, const char* k)
{
    cJSON* it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return (cJSON_IsString(it) && it->valuestring) ? it->valuestring : NULL;
}

/* ------------------------------------------------------------ t.* commands */

static cJSON* t_echo(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    const char* m = hub_sdk_str(args, "msg");
    cJSON* r;
    (void)ctx;
    if (!m) { sprintf(hint, "msg required"); return NULL; }
    r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "echo", m);
    return r;
}

static cJSON* t_fail(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    (void)args;
    ctx->err_code = "T_FAIL";
    sprintf(hint, "deliberate business failure");
    return NULL;
}

static cJSON* t_argcount(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    cJSON* r = cJSON_CreateObject();
    (void)ctx; (void)hint;
    cJSON_AddNumberToObject(r, "n", args ? cJSON_GetArraySize(args) : -1);
    return r;
}

/* b2: observe what the hub put into ctx (_ctx.budget_ms passthrough) */
static int g_seen_budget = -1;
static cJSON* t_ctx(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    cJSON* r = cJSON_CreateObject();
    (void)args; (void)hint;
    g_seen_budget = ctx->timeout_budget_ms;
    cJSON_AddNumberToObject(r, "budget", ctx->timeout_budget_ms);
    cJSON_AddNumberToObject(r, "sdkv", ctx->sdk_version);
    return r;
}

/* v2: async progress from inside a command (entry-path integration) */
static int g_progress_ok = -1;
static cJSON* t_progress(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    cJSON* r;
    (void)args; (void)hint;
    g_progress_ok = hub_sdk_progress(ctx, 7, "inside");
    r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "progress_ok", g_progress_ok);
    cJSON_AddStringToObject(r, "map", ctx->progress_map ? ctx->progress_map : "");
    return r;
}

/* b2: audit-hook collector (hub_sdk_set_audit) */
#define MAX_AUD 16
static char g_audit_lines[MAX_AUD][128];
static int  g_audit_n = 0;
static void audit_collect(const char* line)
{
    if (g_audit_n < MAX_AUD) {
        strncpy(g_audit_lines[g_audit_n], line, 127);
        g_audit_lines[g_audit_n][127] = '\0';
        g_audit_n++;
    }
}
static int audit_seen(const char* needle)
{
    int i;
    for (i = 0; i < g_audit_n; i++)
        if (strstr(g_audit_lines[i], needle)) return 1;
    return 0;
}

static const HubCmdEntry TBL[] = {
    { "t.echo",     t_echo },
    { "t.fail",     t_fail },
    { "t.argcount", t_argcount },
    { "t.ctx",      t_ctx },
    { "t.progress", t_progress },
    { NULL, NULL }
};

/* ------------------------------------------------------------------ main */

int main(void)
{
    ProError ret;
    int n;
    cJSON* o;
    const char* s;

    printf("=== hub_sdk v0/v1 offline test (D12 + M3b b2) ===\n");

    printf("[S1] normal dispatch (STRING in)\n");
    o = run("{\"command\":\"t.echo\",\"args\":{\"msg\":\"hi\"}}", 0, TBL, &ret, &n);
    CHECK(ret == PRO_TK_NO_ERROR, "channel return NO_ERROR");
    CHECK(o && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "ok")), "ok:true");
    s = o ? jstr(cJSON_GetObjectItemCaseSensitive(o, "result"), "echo") : NULL;
    CHECK(s && strcmp(s, "hi") == 0, "result.echo == hi");
    CHECK(o && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o, "ms")), "ms present");
    cJSON_Delete(o);

    printf("[S2] WSTRING input accepted\n");
    o = run("{\"command\":\"t.echo\",\"args\":{\"msg\":\"\xE7\xBD\x91\xE6\xA0\xBC\xE2\x9C\x93\"}}",
            1, TBL, &ret, &n);
    CHECK(ret == PRO_TK_NO_ERROR && o && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "ok")),
          "ok:true via WSTRING IN_JSON");
    s = o ? jstr(cJSON_GetObjectItemCaseSensitive(o, "result"), "echo") : NULL;
    CHECK(s && strcmp(s, "\xE7\xBD\x91\xE6\xA0\xBC\xE2\x9C\x93") == 0, "UTF-8 roundtrip (wangge+check)");
    cJSON_Delete(o);

    printf("[S3] unknown command -> CMD_NOT_FOUND (business, channel still NO_ERROR)\n");
    o = run("{\"command\":\"t.nope\",\"args\":{}}", 0, TBL, &ret, &n);
    CHECK(ret == PRO_TK_NO_ERROR, "channel NO_ERROR");
    s = jstr(o, "err_code");
    CHECK(s && strcmp(s, "CMD_NOT_FOUND") == 0, "err_code CMD_NOT_FOUND");
    CHECK(o && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "ok")), "ok:false");
    cJSON_Delete(o);

    printf("[S4] missing IN_JSON -> SDK_BAD_INPUT\n");
    {
        ProArgument* in = NULL;
        ProArgument* out = NULL;
        ProArrayAlloc(0, sizeof(ProArgument), 1, (ProArray*)&in);  /* empty array */
        ret = hub_sdk_entry(in, &out, TBL);
        CHECK(ret == PRO_TK_NO_ERROR, "channel NO_ERROR");
        o = (out && out[0].value.v.s) ? cJSON_Parse(out[0].value.v.s) : NULL;
        s = jstr(o, "err_code");
        CHECK(s && strcmp(s, "SDK_BAD_INPUT") == 0, "err_code SDK_BAD_INPUT");
        cJSON_Delete(o);
        if (out) ProArgumentProarrayFree(&out);
        ProArgumentProarrayFree(&in);
    }

    printf("[S5] malformed IN_JSON -> SDK_BAD_JSON\n");
    o = run("{not json", 0, TBL, &ret, &n);
    s = jstr(o, "err_code");
    CHECK(ret == PRO_TK_NO_ERROR && s && strcmp(s, "SDK_BAD_JSON") == 0, "err_code SDK_BAD_JSON");
    cJSON_Delete(o);

    printf("[S6] no command field -> SDK_NO_COMMAND\n");
    o = run("{\"args\":{}}", 0, TBL, &ret, &n);
    s = jstr(o, "err_code");
    CHECK(ret == PRO_TK_NO_ERROR && s && strcmp(s, "SDK_NO_COMMAND") == 0, "err_code SDK_NO_COMMAND");
    cJSON_Delete(o);

    printf("[S7] business failure carries ctx err_code + hint\n");
    o = run("{\"command\":\"t.fail\",\"args\":{}}", 0, TBL, &ret, &n);
    s = jstr(o, "err_code");
    CHECK(s && strcmp(s, "T_FAIL") == 0, "err_code T_FAIL (ctx override)");
    s = jstr(o, "hint");
    CHECK(s && strcmp(s, "deliberate business failure") == 0, "hint passthrough");
    cJSON_Delete(o);

    printf("[S8] missing args -> empty object (tolerant)\n");
    o = run("{\"command\":\"t.argcount\"}", 0, TBL, &ret, &n);
    {
        cJSON* r = o ? cJSON_GetObjectItemCaseSensitive(o, "result") : NULL;
        cJSON* nv = r ? cJSON_GetObjectItemCaseSensitive(r, "n") : NULL;
        CHECK(nv && cJSON_IsNumber(nv) && nv->valueint == 0, "args seen as empty object (n=0)");
    }
    cJSON_Delete(o);

    printf("[S9] unlabeled string fallback\n");
    {
        ProArgument* in = mk_inputs(L"SOMETHING_ELSE",
                                    "{\"command\":\"t.echo\",\"args\":{\"msg\":\"fb\"}}", 0);
        ProArgument* out = NULL;
        ret = hub_sdk_entry(in, &out, TBL);
        o = (out && out[0].value.v.s) ? cJSON_Parse(out[0].value.v.s) : NULL;
        s = o ? jstr(cJSON_GetObjectItemCaseSensitive(o, "result"), "echo") : NULL;
        CHECK(ret == PRO_TK_NO_ERROR && s && strcmp(s, "fb") == 0, "first-string fallback dispatches");
        cJSON_Delete(o);
        if (out) ProArgumentProarrayFree(&out);
        ProArgumentProarrayFree(&in);
    }

    printf("[S10] label hit beats earlier unlabeled string\n");
    {
        ProArgument* in = NULL;
        ProArgument a1, a2;
        ProArgument* out = NULL;
        ProArrayAlloc(0, sizeof(ProArgument), 1, (ProArray*)&in);
        memset(&a1, 0, sizeof(a1)); wcscpy(a1.label, L"JUNK");
        ProValuedataStringSet(&a1.value, (char*)"{{{ not json");
        ProArrayObjectAdd((ProArray*)&in, -1, 1, &a1);
        memset(&a2, 0, sizeof(a2)); wcscpy(a2.label, L"IN_JSON");
        ProValuedataStringSet(&a2.value, (char*)"{\"command\":\"t.echo\",\"args\":{\"msg\":\"lbl\"}}");
        ProArrayObjectAdd((ProArray*)&in, -1, 1, &a2);
        ret = hub_sdk_entry(in, &out, TBL);
        o = (out && out[0].value.v.s) ? cJSON_Parse(out[0].value.v.s) : NULL;
        s = o ? jstr(cJSON_GetObjectItemCaseSensitive(o, "result"), "echo") : NULL;
        CHECK(ret == PRO_TK_NO_ERROR && s && strcmp(s, "lbl") == 0, "IN_JSON label wins over junk");
        cJSON_Delete(o);
        if (out) ProArgumentProarrayFree(&out);
        ProArgumentProarrayFree(&in);
    }

    printf("[S11] NULL outputs -> channel BAD_INPUTS\n");
    {
        ProArgument* in = mk_inputs(L"IN_JSON", "{\"command\":\"t.echo\",\"args\":{\"msg\":\"x\"}}", 0);
        ret = hub_sdk_entry(in, NULL, TBL);
        CHECK(ret == PRO_TK_BAD_INPUTS, "BAD_INPUTS (only legal channel error besides OOM)");
        ProArgumentProarrayFree(&in);
    }

    printf("[S12] NULL table -> SDK_BAD_TABLE\n");
    o = run("{\"command\":\"t.echo\",\"args\":{\"msg\":\"x\"}}", 0, NULL, &ret, &n);
    s = jstr(o, "err_code");
    CHECK(ret == PRO_TK_NO_ERROR && s && strcmp(s, "SDK_BAD_TABLE") == 0, "err_code SDK_BAD_TABLE");
    cJSON_Delete(o);

    printf("[S13] v1: _ctx.budget_ms passthrough + time_left helper (M3b)\n");
    o = run("{\"command\":\"t.ctx\",\"args\":{},\"_ctx\":{\"budget_ms\":4321}}", 0, TBL, &ret, &n);
    {
        cJSON* r = o ? cJSON_GetObjectItemCaseSensitive(o, "result") : NULL;
        cJSON* b = r ? cJSON_GetObjectItemCaseSensitive(r, "budget") : NULL;
        cJSON* v = r ? cJSON_GetObjectItemCaseSensitive(r, "sdkv") : NULL;
        CHECK(b && b->valueint == 4321, "_ctx.budget_ms reaches handler (4321)");
        CHECK(v && v->valueint == HUB_SDK_VERSION, "ctx.sdk_version == HUB_SDK_VERSION");
        CHECK(g_seen_budget == 4321, "handler observed budget via ctx");
    }
    cJSON_Delete(o);
    o = run("{\"command\":\"t.ctx\",\"args\":{}}", 0, TBL, &ret, &n);
    {
        cJSON* r = o ? cJSON_GetObjectItemCaseSensitive(o, "result") : NULL;
        cJSON* b = r ? cJSON_GetObjectItemCaseSensitive(r, "budget") : NULL;
        CHECK(b && b->valueint == 0 && g_seen_budget == 0,
              "no _ctx -> budget 0 (unknown/old hub)");
    }
    cJSON_Delete(o);
    {
        HubCmdCtx c;
        memset(&c, 0, sizeof(c));
        c.sdk_version = HUB_SDK_VERSION;
        c.start_tick = GetTickCount64();
        CHECK(hub_sdk_time_left_ms(&c) == -1, "time_left: no budget -> -1 (unknown)");
        CHECK(hub_sdk_time_left_ms(NULL) == -1, "time_left: NULL ctx -> -1");
        c.timeout_budget_ms = 5000;
        CHECK(hub_sdk_time_left_ms(&c) > 4000, "time_left: fresh 5s budget ~5000");
        c.start_tick -= 6000;
        CHECK(hub_sdk_time_left_ms(&c) == 0, "time_left: budget spent -> 0 (yield now)");
    }

    printf("[S14] v1 R2: armed-callback ledger (M3b b2)\n");
    {
        char rep[128];
        CHECK(hub_sdk_armed_count() == 0, "ledger starts empty");
        hub_sdk_arm((void*)(uintptr_t)0xA1, "uitimer");
        hub_sdk_arm((void*)(uintptr_t)0xA2, "notify");
        CHECK(hub_sdk_armed_count() == 2, "two arms counted");
        hub_sdk_arm((void*)(uintptr_t)0xA1, "uitimer2");
        CHECK(hub_sdk_armed_count() == 2, "re-arm same handle refreshes (no dup)");
        CHECK(hub_sdk_terminate_check(rep, sizeof(rep)) == 2
              && strstr(rep, "uitimer2") && strstr(rep, "notify"),
              "terminate_check reports 2 with kinds");
        hub_sdk_disarm((void*)(uintptr_t)0xA1);
        hub_sdk_disarm((void*)(uintptr_t)0xA2);
        CHECK(hub_sdk_armed_count() == 0, "disarm clears");
        CHECK(hub_sdk_terminate_check(rep, sizeof(rep)) == 0, "clean terminate_check -> 0");
    }

    printf("[S15] v1: audit hook lines (dispatch HIT/MISS, biz-fail, ok, R2)\n");
    hub_sdk_set_audit(audit_collect);
    o = run("{\"command\":\"t.echo\",\"args\":{\"msg\":\"a\"}}", 0, TBL, &ret, &n);
    cJSON_Delete(o);
    o = run("{\"command\":\"t.nope\",\"args\":{}}", 0, TBL, &ret, &n);
    cJSON_Delete(o);
    o = run("{\"command\":\"t.fail\",\"args\":{}}", 0, TBL, &ret, &n);
    cJSON_Delete(o);
    CHECK(audit_seen("dispatch HIT cmd=t.echo"), "audit: dispatch HIT");
    CHECK(audit_seen("dispatch MISS cmd=t.nope"), "audit: dispatch MISS");
    CHECK(audit_seen("biz-fail cmd=t.fail err_code=T_FAIL"), "audit: biz-fail line");
    CHECK(audit_seen("ok cmd=t.echo ms="), "audit: ok+ms line");
    CHECK(hub_sdk_terminate_check(NULL, 0) == 0 && audit_seen("R2 terminate: clean"),
          "audit: R2 terminate line through hook");
    hub_sdk_set_audit(NULL);

    printf("[S16] v2: async progress slot (shared-memory contract, docs\\39 3.3)\n");
    {
        char name[96];
        HANDLE hm;
        HubJobSlot* slot;
        HubCmdCtx c;

        sprintf(name, "creo_hub_sdk_test_progress_%lu", (unsigned long)GetCurrentProcessId());
        hm = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                sizeof(HubJobSlot), name);
        CHECK(hm != NULL, "slot mapping created");
        slot = hm ? (HubJobSlot*)MapViewOfFile(hm, FILE_MAP_ALL_ACCESS, 0, 0,
                                               sizeof(HubJobSlot)) : NULL;
        CHECK(slot != NULL, "slot view mapped");
        if (slot) {
            memset(slot, 0, sizeof(*slot));
            slot->magic = HUB_JOBSLOT_MAGIC;
            memset(&c, 0, sizeof(c));
            c.sdk_version = HUB_SDK_VERSION;
            c.progress_map = name;

            CHECK(hub_sdk_progress(&c, 42, "step 42") == 1, "progress write -> 1");
            CHECK(slot->seq % 2 == 0, "seq even after publish (stable)");
            CHECK(slot->state == HUB_JOBSLOT_ST_WORKING && slot->pct == 42
                  && strcmp(slot->msg, "step 42") == 0, "slot carries state/pct/msg");

            CHECK(hub_sdk_progress(&c, -5, NULL) == 1, "indeterminate write -> 1");
            CHECK(slot->pct == HUB_JOBSLOT_PCT_UNKNOWN && slot->msg[0] == '\0',
                  "pct below -1 clamps to -1, empty msg");

            CHECK(hub_sdk_progress(&c, 200, NULL) == 1, "over-100 write -> 1");
            CHECK(slot->pct == 100, "pct above 100 clamps to 100");

            CHECK(hub_sdk_progress(NULL, 1, "x") == 0, "NULL ctx -> 0 (no-op)");
            {
                HubCmdCtx c2;
                memset(&c2, 0, sizeof(c2));
                CHECK(hub_sdk_progress(&c2, 1, "x") == 0, "no progress_map -> 0 (no-op)");
                c2.progress_map = "creo_hub_sdk_test_missing_map";
                CHECK(hub_sdk_progress(&c2, 1, "x") == 0, "missing mapping -> 0 (no-op)");
            }

            /* entry-path integration: _ctx.progress_map -> ctx -> command writes */
            {
                char pj[256];
                ProError ret = PRO_TK_NO_ERROR;
                int n = 0;
                cJSON* o;
                _snprintf(pj, sizeof(pj),
                          "{\"command\":\"t.progress\",\"args\":{},"
                          "\"_ctx\":{\"progress_map\":\"%s\"}}", name);
                pj[sizeof(pj) - 1] = '\0';
                o = run(pj, 0, TBL, &ret, &n);
                CHECK(ret == PRO_TK_NO_ERROR && o && cJSON_IsTrue(
                          cJSON_GetObjectItemCaseSensitive(o, "ok")),
                      "t.progress entry call ok");
                CHECK(g_progress_ok == 1, "command saw a writable slot");
                CHECK(slot->pct == 7 && strcmp(slot->msg, "inside") == 0,
                      "slot updated through entry path (pct=7, msg=inside)");
                cJSON_Delete(o);
            }
        }
        if (slot) UnmapViewOfFile(slot);
        if (hm) CloseHandle(hm);
    }

    printf("\n=== sdk_test: %d PASS / %d FAIL ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
