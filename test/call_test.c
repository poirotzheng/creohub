/* =====================================================================
 * creo-hub M3b b1 OFFLINE call_test: /call validation chain + executor
 * unit tests (docs\25 §2 b1 gates). Hermetic temp root; the stub adapter
 * (test_stubs.c) stages channel results and OUT_JSON. No Creo, no PTK libs.
 * Exit: 0 = ALL PASS, 1 = failures, 2 = setup error.
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ProToolkit.h>
#include "registry.h"
#include "state.h"
#include "call_exec.h"
#include "adapter_ptk.h"
#include "nonce.h"
#include "opmode.h"
#include "cJSON.h"

/* test_stubs.c control surface */
extern char g_root[MAX_PATH];
extern AdapterChannelStatus g_stub_ch;
extern char     g_stub_ch_name[64];
extern char     g_stub_out_json[8192];
extern ProError g_stub_fret;
extern wchar_t  g_stub_mdl_name[64];

static char g_troot[MAX_PATH];
static int g_pass = 0, g_fail = 0;

static void CHECK(int cond, const char* what)
{
    if (cond) { g_pass++; printf("  [PASS] %s\n", what); }
    else      { g_fail++; printf("  [FAIL] %s\n", what); }
}

/* ------------------------------------------------------------ fs helpers */

static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    char* p;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\') { *p = '\0'; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    }
    CreateDirectoryA(tmp, NULL);
}

static void write_file(const char* path, const char* content)
{
    FILE* f = fopen(path, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

static void mkpkg(const char* dir, const char* entry_name, const char* manifest)
{
    char p[MAX_PATH + 128], json[MAX_PATH + 64];
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\bin", g_troot, dir);                 mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\text\\text\\usascii", g_troot, dir); mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\bin\\%s", g_troot, dir, entry_name);
    write_file(p, "MZ-fake");
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\text\\text\\usascii\\%s_messages.txt", g_troot, dir, dir);
    write_file(p, "#MSG dummy\n");
    _snprintf(json, sizeof(json), "%s\\plugins\\%s\\plugin.json", g_troot, dir);
    write_file(json, manifest);
}

#define CT_MANIFEST \
    "{\"manifestVersion\":2,\"id\":\"ct\",\"name\":\"Call test pkg\",\"version\":\"1.0.0\"," \
    "\"adapter\":\"ptk-sync\",\"entry\":\"bin/ct.dll\",\"textDir\":\"text\",\"commands\":[" \
    "{\"id\":\"ct.echo\",\"title\":\"Echo\",\"desc\":\"echo msg\",\"descForLLM\":\"echo back\"," \
     "\"risk\":\"safe\",\"context\":\"part\",\"idempotent\":true," \
     "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\"}}," \
     "\"required\":[\"msg\"],\"additionalProperties\":false}}," \
    "{\"id\":\"ct.pick\",\"title\":\"Pick\",\"desc\":\"pick mode\",\"descForLLM\":\"pick mode\"," \
     "\"risk\":\"safe\"," \
     "\"inputSchema\":{\"type\":\"object\",\"properties\":{" \
     "\"mode\":{\"type\":\"string\",\"enum\":[\"fast\",\"slow\"]}," \
     "\"n\":{\"type\":\"number\"},\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}," \
     "\"required\":[\"mode\"],\"additionalProperties\":false}}," \
    "{\"id\":\"ct.slow\",\"title\":\"Slow\",\"desc\":\"slow op\",\"descForLLM\":\"slow op\"," \
     "\"risk\":\"safe\",\"timeoutSec\":3,\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}," \
    "{\"id\":\"ct.conf\",\"title\":\"Conf\",\"desc\":\"confirm op\",\"descForLLM\":\"confirm op\"," \
     "\"risk\":\"confirm\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}," \
    "{\"id\":\"ct.destr\",\"title\":\"Destr\",\"desc\":\"destructive op\",\"descForLLM\":\"destructive op\"," \
     "\"risk\":\"destructive\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}"

#define CT2_MANIFEST \
    "{\"manifestVersion\":2,\"id\":\"ct2\",\"name\":\"Route A pkg\",\"version\":\"1.0.0\"," \
    "\"adapter\":\"ptk-sync\",\"entry\":\"bin/ct2.dll\",\"textDir\":\"text\"," \
    "\"hubCmdExport\":\"custom_entry\",\"callRoute\":\"A\",\"commands\":[" \
    "{\"id\":\"ct2.run\",\"title\":\"Run\",\"desc\":\"run\",\"descForLLM\":\"run\"," \
     "\"risk\":\"safe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}"

#define CTBAD_MANIFEST \
    "{\"manifestVersion\":2,\"id\":\"ctbad\",\"name\":\"Bad route pkg\",\"version\":\"1.0.0\"," \
    "\"adapter\":\"ptk-sync\",\"entry\":\"bin/ctbad.dll\",\"textDir\":\"text\",\"callRoute\":\"X\"," \
    "\"commands\":[]}"

static void setup_root(void)
{
    char sub[MAX_PATH], st[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, sub);
    sub[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_call", sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins", g_troot);   mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\logs", g_troot);      mkdir_p(sub);
    mkpkg("ct",    "ct.dll",    CT_MANIFEST);
    mkpkg("ct2",   "ct2.dll",   CT2_MANIFEST);
    mkpkg("ctbad", "ctbad.dll", CTBAD_MANIFEST);
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
    strcpy(g_root, g_troot);
}

/* ------------------------------------------------------------ helpers */

static void stub_reset(void)
{
    g_stub_ch = ADP_CH_OK;
    strcpy(g_stub_ch_name, "OK");
    g_stub_out_json[0] = '\0';
    g_stub_fret = PRO_TK_NO_ERROR;
    wcscpy(g_stub_mdl_name, L"my_part");
}

static char g_resp[16384];
static CallTarget g_tgt;

/* run precheck; returns its rc (1=enqueue, 0=fast-fail), fills g_resp/g_tgt */
static int pc(const char* body_json, int* status)
{
    cJSON* b = cJSON_Parse(body_json);
    int rc;
    g_resp[0] = '\0';
    memset(&g_tgt, 0, sizeof(g_tgt));
    if (!b) { *status = -1; return 0; }
    rc = call_exec_precheck(b, &g_tgt, g_resp, sizeof(g_resp), status);
    cJSON_Delete(b);
    if (rc == 1) *status = 0;   /* pass leaves *status untouched by contract */
    return rc;
}

static char g_out[16384];
static char g_idout[64];
static char g_stateout[160];   /* b2: stateout grew (txn/nonce_id fragments) */

static int ex(const char* body_json)
{
    g_out[0] = '\0'; g_idout[0] = '\0'; g_stateout[0] = '\0';
    return call_exec_execute(body_json, g_out, sizeof(g_out), g_idout, g_stateout);
}

static int has(const char* needle) { return strstr(g_resp, needle) != NULL; }
static int outhas(const char* needle) { return strstr(g_out, needle) != NULL; }

/* b2 time seam for the nonce TTL tests */
static unsigned long long g_fake_now = 5000000ULL;
static unsigned long long fake_now_fn(void) { return g_fake_now; }

/* pull needConfirm.{transactionId,nonce} out of the last precheck response */
static int grab_ticket(char txn[64], char nonce[64])
{
    cJSON* rj = cJSON_Parse(g_resp);
    cJSON* nc = rj ? cJSON_GetObjectItemCaseSensitive(rj, "needConfirm") : NULL;
    const cJSON* jt = nc ? cJSON_GetObjectItemCaseSensitive(nc, "transactionId") : NULL;
    const cJSON* jn = nc ? cJSON_GetObjectItemCaseSensitive(nc, "nonce") : NULL;
    int ok = (jt && jn && cJSON_IsString(jt) && cJSON_IsString(jn));
    if (ok) {
        strncpy(txn, jt->valuestring, 63);   txn[63] = '\0';
        strncpy(nonce, jn->valuestring, 63); nonce[63] = '\0';
    }
    cJSON_Delete(rj);
    return ok;
}

/* ------------------------------------------------------------ main */

int main(void)
{
    int ict, ict2, ibad, status = 0, rc;

    printf("== creo-hub M3b b1 call_test (offline unit) ==\n");
    setup_root();
    registry_scan(g_root);

    printf("--- setup ---\n");
    CHECK(g_n_plugins == 3, "fixtures scanned: 3 packages");
    ict  = registry_find_idx("ct");
    ict2 = registry_find_idx("ct2");
    ibad = registry_find_idx("ctbad");
    CHECK(ict >= 0 && ict2 >= 0 && ibad >= 0, "ct/ct2/ctbad found");
    CHECK(g_plugins[ibad].state == PS_INVALID,
          "ctbad callRoute 'X' rejected INVALID at parse (typed field)");
    CHECK(strcmp(g_plugins[ict2].hub_cmd_export, "custom_entry") == 0,
          "ct2 hubCmdExport parsed");
    CHECK(strcmp(g_plugins[ict2].call_route, "A") == 0, "ct2 callRoute A parsed");
    CHECK(strcmp(g_plugins[ict].cmdv[0].context, "part") == 0,
          "ct.echo context 'part' parsed");
    CHECK(strcmp(g_plugins[ict].cmdv[1].context, "any") == 0,
          "ct.pick context defaults to 'any' (P-b1-1)");

    CHECK(registry_mount(ict, 1) == PRO_TK_NO_ERROR, "ct mounted (stub adapter)");
    CHECK(g_plugins[ict].state == PS_ACTIVE, "ct ACTIVE");
    CHECK(registry_mount(ict2, 1) == PRO_TK_NO_ERROR, "ct2 mounted");

    printf("--- precheck: 404/409 gates ---\n");
    rc = pc("{\"plugin\":\"nope\",\"command\":\"x.y\"}", &status);
    CHECK(rc == 0 && status == 404 && has("plugin not found"), "unknown plugin -> 404");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.nope\"}", &status);
    CHECK(rc == 0 && status == 404 && has("command not found") && has("ct.echo"),
          "unknown command -> 404 + hint lists commands");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.echo\"}", &status);
    CHECK(rc == 0 && status == 422 && has("args.msg") && has("required"),
          "missing required arg -> 422 details path args.msg");

    printf("--- precheck: schema value domain (422) ---\n");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":5}}", &status);
    CHECK(rc == 0 && status == 422 && has("expected string"), "type mismatch -> 422");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"a\",\"zzz\":1}}", &status);
    CHECK(rc == 0 && status == 422 && has("additionalProperties"), "extra key -> 422");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.pick\",\"args\":{\"mode\":\"turbo\"}}", &status);
    CHECK(rc == 0 && status == 422 && has("not in enum"), "enum violation -> 422");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.pick\",\"args\":{\"mode\":\"fast\",\"tags\":[\"a\",1]}}", &status);
    CHECK(rc == 0 && status == 422 && has("tags[1]"), "array items violation -> 422 path tags[1]");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.pick\",\"args\":{\"mode\":\"fast\",\"n\":2,\"tags\":[\"a\"]}}", &status);
    CHECK(rc == 1 && status == 0 && g_tgt.timeout_ms == 10000,
          "valid args pass; default budget 10000ms");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.slow\"}", &status);
    CHECK(rc == 1 && g_tgt.timeout_ms == 3000, "timeoutSec 3 -> budget 3000ms");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":null}", &status);
    CHECK(rc == 0 && status == 422, "args:null with required -> 422 (empty object semantics)");

    printf("--- precheck: risk gate (428 + nonce issuance, b2) ---\n");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
    CHECK(rc == 0 && status == 428 && has("confirmation required")
          && has("transactionId") && !has("confirmVia"),
          "confirm risk -> 428 needConfirm (no confirmVia)");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}", &status);
    CHECK(rc == 0 && status == 428 && has("panel-required"),
          "destructive -> 428 + confirmVia panel-required");

    printf("--- precheck: state gate (409) ---\n");
    CHECK(registry_unmount(ict, 1) == PRO_TK_NO_ERROR, "ct unmounted for 409 test");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"a\"}}", &status);
    CHECK(rc == 0 && status == 409 && has("plugin not active") && has("REGISTERED"),
          "inactive -> 409 + state (gate before channel)");
    CHECK(registry_mount(ict, 1) == PRO_TK_NO_ERROR, "ct remounted");

    printf("--- execute: success + state token ---\n");
    stub_reset();
    strcpy(g_stub_out_json, "{\"ok\":true,\"result\":{\"echo\":\"hi\"}}");
    status = ex("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"hi\"}}");
    CHECK(status == 200 && outhas("\"ok\":true") && outhas("\"result\"")
          && outhas("\"echo\":\"hi\"") && outhas("\"ms\""),
          "success -> 200 ok:true + result raw + ms");
    CHECK(outhas("\"model\":\"my_part\"") && outhas("\"regen\""),
          "state token {model,regen} present");
    CHECK(strcmp(g_idout, "ct") == 0 && strstr(g_stateout, "result=ok") != NULL,
          "audit fields idout/stateout filled");

    printf("--- execute: business failure passthrough (200) ---\n");
    stub_reset();
    strcpy(g_stub_out_json,
           "{\"ok\":false,\"err_code\":\"CT_FAIL\",\"err_msg\":\"m\",\"hint\":\"h\"}");
    status = ex("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"x\"}}");
    CHECK(status == 200 && outhas("CT_FAIL") && outhas("\"hint\":\"h\"") && outhas("\"ms\""),
          "business fail -> 200 passthrough err_code/hint + ms");

    printf("--- execute: infrastructure failures (500) ---\n");
    stub_reset();
    g_stub_ch = ADP_CH_PROCADDR_NULL;
    strcpy(g_stub_ch_name, "PROCADDR_NULL");
    status = ex("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"x\"}}");
    CHECK(status == 500 && outhas("channel:PROCADDR_NULL") && outhas("\"retryable\":false"),
          "channel fail -> 500 channel:* + retryable false");
    stub_reset();
    status = ex("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"x\"}}");
    CHECK(status == 500 && outhas("no OUT_JSON"), "missing OUT_JSON -> 500");
    stub_reset();
    strcpy(g_stub_out_json, "{{{ not json");
    status = ex("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"x\"}}");
    CHECK(status == 500 && outhas("not parseable"), "garbage OUT_JSON -> 500");

    printf("--- execute: TOCTOU + route A + no-model ---\n");
    stub_reset();
    CHECK(registry_unmount(ict, 1) == PRO_TK_NO_ERROR, "ct unmounted for TOCTOU test");
    status = ex("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"x\"}}");
    CHECK(status == 409 && outhas("queued"), "state drift while queued -> 409");
    CHECK(registry_mount(ict, 1) == PRO_TK_NO_ERROR, "ct remounted");
    stub_reset();
    strcpy(g_stub_out_json, "{\"ok\":true,\"result\":{\"ran\":true}}");
    status = ex("{\"plugin\":\"ct2\",\"command\":\"ct2.run\"}");
    CHECK(status == 200 && outhas("\"ran\":true"), "route A package executes via stub");
    stub_reset();
    g_stub_mdl_name[0] = L'\0';
    strcpy(g_stub_out_json, "{\"ok\":true,\"result\":{}}");
    status = ex("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"x\"}}");
    CHECK(status == 200 && outhas("\"model\":null"), "no current model -> state.model null");

    printf("--- b2: dryRun precheck (full chain, never enqueued) ---\n");
    stub_reset();
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.echo\",\"args\":{\"msg\":\"d\"},\"dryRun\":true}", &status);
    CHECK(rc == 0 && status == 200 && has("\"dryRun\":true") && has("\"wouldExecute\"")
          && has("\"needsConfirm\":false") && has("argsDigest"),
          "dryRun safe -> 200 wouldExecute needsConfirm:false + argsDigest");
    rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.destr\",\"dryRun\":true}", &status);
    CHECK(rc == 0 && status == 200 && has("\"needsConfirm\":true") && has("needConfirm")
          && has("panel-required") && has("transactionId"),
          "dryRun destructive -> needsConfirm:true + nonce pre-issued + panel-required");

    printf("--- b2: nonce lifecycle via precheck/execute ---\n");
    {
        unsigned long long (*saved_now)(void) = nonce_now_fn;
        char txn[64], nonce[64], body[512];
        nonce_now_fn = fake_now_fn;
        stub_reset();
        strcpy(g_stub_out_json, "{\"ok\":true,\"result\":{\"done\":1}}");

        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(rc == 0 && status == 428 && has("confirmation required")
              && has("transactionId") && has("\"nonce\"") && has("argsDigest")
              && has("\"expiresMs\":60000") && !has("confirmVia"),
              "first call -> 428 full frozen shape (confirm level, no confirmVia)");
        CHECK(grab_ticket(txn, nonce) && strlen(nonce) == 32,
              "needConfirm carries txn + 32-hex nonce");

        _snprintf(body, sizeof(body),
                  "{\"plugin\":\"ct\",\"command\":\"ct.conf\",\"confirm_nonce\":\"%s\"}", nonce);
        rc = pc(body, &status);
        CHECK(rc == 0 && status == 409 && has("NONCE_PENDING_APPROVAL"),
              "nonce retry BEFORE approval -> 409 PENDING_APPROVAL");

        CHECK(nonce_approve_txn(txn) == NR_OK, "approval registered (as /api/v1/confirm does)");
        rc = pc(body, &status);
        CHECK(rc == 1 && status == 0, "approved nonce -> precheck pass");
        CHECK(nonce_verify(nonce, ict, g_tgt.cidx, nonce_args_digest(NULL)) == NR_OK,
              "still APPROVED after precheck (burn at execute only, 启示 11)");

        status = ex(body);
        CHECK(status == 200 && outhas("\"done\":1")
              && strstr(g_stateout, "txn=") != NULL
              && strstr(g_stateout, "nonce_id=") != NULL,
              "execute consumes nonce; audit fragment carries txn + nonce_id");

        rc = pc(body, &status);
        CHECK(rc == 0 && status == 428 && has("previous nonce consumed"),
              "consumed replay -> 428 with FRESH nonce (docs22 6.3)");

        /* args binding: approve digest(no-args), then present args {x:1} */
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(status == 428 && grab_ticket(txn, nonce), "re-issue for mismatch test");
        CHECK(nonce_approve_txn(txn) == NR_OK, "approve (args = none)");
        _snprintf(body, sizeof(body),
                  "{\"plugin\":\"ct\",\"command\":\"ct.conf\",\"args\":{\"x\":1},\"confirm_nonce\":\"%s\"}", nonce);
        rc = pc(body, &status);
        CHECK(rc == 0 && status == 409 && has("NONCE_ARGS_MISMATCH"),
              "approved A-args, submitted B-args -> 409 NONCE_ARGS_MISMATCH");

        /* auto-pair: destructive approved in-panel, retry WITHOUT nonce */
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}", &status);
        CHECK(status == 428 && has("panel-required") && grab_ticket(txn, nonce),
              "destructive -> 428 confirmVia panel-required");
        CHECK(nonce_approve_txn(txn) == NR_OK, "panel approval registered");
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}", &status);
        CHECK(rc == 1 && status == 0, "auto-pair: approved triple passes WITHOUT nonce");
        status = ex("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}");
        CHECK(status == 200 && outhas("\"done\":1"), "auto-pair execute consumes approval");
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}", &status);
        CHECK(rc == 0 && status == 428, "after burn: fresh confirmation required again");

        /* TTL expiry through the time seam. NOTE: the mismatch test left a
         * live APPROVED ct.conf slot (digest of no-args) - auto-pair would
         * (correctly!) swallow the next bare call, so burn it first. */
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        if (rc == 1) ex("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}");
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(status == 428 && grab_ticket(txn, nonce), "issue for TTL test");
        CHECK(nonce_approve_txn(txn) == NR_OK, "approve for TTL test");
        g_fake_now += NONCE_TTL_MS + 1000;   /* 61s post-issue: S1b grace live */
        _snprintf(body, sizeof(body),
                  "{\"plugin\":\"ct\",\"command\":\"ct.conf\",\"confirm_nonce\":\"%s\"}", nonce);
        rc = pc(body, &status);
        CHECK(rc == 1, "S1b: approved retry 61s post-issue passes precheck (grace window)");
        status = ex(body);
        CHECK(status == 200 && outhas("\"done\":1"),
              "S1b: execute consumes the approved slot and succeeds (race fix)");
        /* bare-retry arm = the live L3 flow (nonce never reaches the agent):
         * fresh ticket, approve, +61s, BARE call must pass precheck via peek. */
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(status == 428 && grab_ticket(txn, nonce), "issue for bare-retry arm");
        CHECK(nonce_approve_txn(txn) == NR_OK, "approve for bare-retry arm");
        g_fake_now += NONCE_TTL_MS + 1000;
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(rc == 1,
              "S1b: BARE retry 61s post-issue passes precheck via peek (audit fix)");
        status = ex("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}");
        CHECK(status == 200 && outhas("\"done\":1"),
              "S1b: bare-retry auto-pair executes past old 60s cap (live-flow race fix)");
        /* grace cap: fresh ticket, approve, jump past 10 min post-approval */
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(status == 428 && grab_ticket(txn, nonce), "issue for grace-cap test");
        CHECK(nonce_approve_txn(txn) == NR_OK, "approve for grace-cap test");
        g_fake_now += NONCE_APPROVED_TTL_MS + 1000;
        _snprintf(body, sizeof(body),
                  "{\"plugin\":\"ct\",\"command\":\"ct.conf\",\"confirm_nonce\":\"%s\"}", nonce);
        rc = pc(body, &status);
        CHECK(rc == 0 && status == 409 && has("NONCE_EXPIRED"),
              "grace cap over -> 409 NONCE_EXPIRED (fresh first call re-issues)");
        g_fake_now += 1000;
        nonce_now_fn = saved_now;
    }

    printf("--- M3e Q1: operator mode menu (auto-pass gate) ---\n");
    {
        stub_reset();
        strcpy(g_stub_out_json, "{\"ok\":true,\"result\":{\"done\":1}}");
        opmode_set(OPMODE_STANDARD, 0, 0);
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(rc == 0 && status == 428, "M0: confirm still 428 (default unchanged)");
        CHECK(opmode_set(OPMODE_TRUST, 0, 0) == 0, "arm M1 session trust");
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(rc == 1, "M1: confirm auto-passes precheck (no nonce gate)");
        status = ex("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}");
        CHECK(status == 200 && outhas("\"done\":1"), "M1: confirm executes without nonce");
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}", &status);
        CHECK(rc == 0 && status == 428, "M1: destructive STILL panel-required");
        CHECK(opmode_set(OPMODE_UNATTENDED, 600000, 0) == -1,
              "M2 arm without ack rejected (explicit consent mandatory)");
        CHECK(opmode_set(OPMODE_UNATTENDED, 600000, 1) == 0, "arm M2 with ack");
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}", &status);
        CHECK(rc == 1, "M2: destructive auto-passes precheck inside window");
        status = ex("{\"plugin\":\"ct\",\"command\":\"ct.destr\"}");
        CHECK(status == 200, "M2: destructive executes unattended (audited)");
        CHECK(opmode_set(OPMODE_STANDARD, 0, 0) == 0, "disarm back to M0");
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(rc == 0 && status == 428, "M0 restored: confirm 428 again");
    }

    printf("--- b2: state change invalidates approvals ---\n");
    {
        char txn[64], nonce[64], body[512];
        stub_reset();
        rc = pc("{\"plugin\":\"ct\",\"command\":\"ct.conf\"}", &status);
        CHECK(status == 428 && grab_ticket(txn, nonce), "issue for invalidation test");
        CHECK(nonce_approve_txn(txn) == NR_OK, "approve for invalidation test");
        CHECK(registry_unmount(ict, 1) == PRO_TK_NO_ERROR, "unmount ct (state change)");
        _snprintf(body, sizeof(body),
                  "{\"plugin\":\"ct\",\"command\":\"ct.conf\",\"confirm_nonce\":\"%s\"}", nonce);
        rc = pc(body, &status);
        CHECK(rc == 0 && status == 409, "post-unmount: nonce dead / state gate rejects");
        CHECK(registry_mount(ict, 1) == PRO_TK_NO_ERROR, "remount ct");
    }

    printf("\n== call_test: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
