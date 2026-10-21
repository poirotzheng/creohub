/* =====================================================================
 * creo-hub M2 OFFLINE test: web_api.c (gates O4/O5/O9/O12/O13 + O8 api
 * layer). Real httpd + real sockets + a PUMP THREAD playing the Creo main
 * thread (executor side of the G1 envelope queue). Hermetic temp root with
 * 3 fixtures: p-ok (v2, 1 safe command), p-fail (stub adapter fails),
 * p-bad (invalid manifest). Audit lines are teed to a file via
 * g_hub_log_tee (test_stubs) and asserted per R3 format.
 * Exit code = number of failures (0 = ALL PASS).
 * ===================================================================== */
#include "web_api.h"
#include "hub_log.h"
#include "registry.h"
#include "reconcile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int g_fail = 0;
static int g_pass = 0;

extern int   g_hub_log_quiet;
extern FILE* g_hub_log_tee;
/* b2: call-face stub control (test_stubs.c) */
extern char  g_stub_out_json[8192];
extern int   g_stub_sleep_ms;

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

#define BASE_PORT 18780
static const char* TOKEN = "***";
static char g_troot[MAX_PATH];
static char g_tee_path[MAX_PATH + 32];
static char g_resp[96 * 1024];
static char g_req[16384];
static int  g_port = 0;
static volatile LONG g_pump_run = 1;
static volatile LONG g_pump_on  = 1;

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

static char* read_file_alloc(const char* path)
{
    FILE* f = fopen(path, "rb");
    long sz;
    char* buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void mkpkg(const char* dir, const char* entry_name, const char* manifest)
{
    char p[MAX_PATH + 128], json[MAX_PATH + 64];
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\bin", g_troot, dir);            mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\text\\text\\usascii", g_troot, dir); mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\bin\\%s", g_troot, dir, entry_name);
    write_file(p, "MZ-fake");
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\text\\text\\usascii\\%s_messages.txt", g_troot, dir, dir);
    write_file(p, "#MSG dummy\n");
    _snprintf(json, sizeof(json), "%s\\plugins\\%s\\plugin.json", g_troot, dir);
    write_file(json, manifest);
}

static void setup_root(void)
{
    char sub[MAX_PATH], st[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, sub);
    sub[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_api", sub);
    {   /* wipe stale fixture tree (T19/T20 add packages at runtime) */
        char cmd[MAX_PATH + 32];
        _snprintf(cmd, sizeof(cmd), "cmd /c rd /s /q \"%s\" >nul 2>&1", g_troot);
        system(cmd);
    }
    _snprintf(sub, sizeof(sub), "%s\\plugins", g_troot);   mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\logs", g_troot);      mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\web", g_troot);       mkdir_p(sub);
    mkpkg("p-ok", "p-ok.dll",
          "{\"manifestVersion\":2,\"id\":\"p-ok\",\"name\":\"OK pkg\",\"version\":\"1.0.0\","
          "\"adapter\":\"ptk-sync\",\"entry\":\"bin/p-ok.dll\",\"textDir\":\"text\","
          "\"commands\":[{\"id\":\"p-ok.hello\",\"title\":\"Hello\",\"desc\":\"say hello\","
          "\"descForLLM\":\"Say hello; takes no arguments\",\"risk\":\"safe\","
          "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
          "{\"id\":\"p-ok.slow\",\"title\":\"Slow\",\"desc\":\"slow op\","
          "\"descForLLM\":\"slow op for orphan tests\",\"risk\":\"safe\",\"idempotent\":true,"
          "\"timeoutSec\":1,\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"id\":\"p-ok.risky\",\"title\":\"Risky\",\"desc\":\"confirm op\","
          "\"descForLLM\":\"confirm-level op for nonce tests\",\"risk\":\"confirm\","
          "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
          "{\"id\":\"p-ok.batch\",\"title\":\"Batch\",\"desc\":\"async long task\","
          "\"descForLLM\":\"async long task (M4b job test)\",\"risk\":\"safe\","
          "\"async\":true,\"timeoutSec\":60,\"group\":\"batch\","
          "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}],"
          "\"provides\":[\"ok.svc\"]}");
    mkpkg("p-fail", "demo_fail.dll",
          "{\"manifestVersion\":1,\"id\":\"p-fail\",\"adapter\":\"ptk-sync\","
          "\"entry\":\"bin/demo_fail.dll\",\"textDir\":\"text\"}");
    mkpkg("p-bad", "p-bad.dll", "{\"manifestVersion\":1}");
    /* seed log file for the /log endpoint tests */
    _snprintf(sub, sizeof(sub), "%s\\logs\\hub_m1.log", g_troot);
    write_file(sub, "[t0] [REG] M2-LOGTEST-1\n[t1] [REG] M2-LOGTEST-2\n[t2] [API] M2-LOGTEST-3\n");
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
    strcpy(g_root, g_troot);
    _snprintf(g_tee_path, sizeof(g_tee_path), "%s\\audit_tee.log", g_troot);
    remove(g_tee_path);
}

/* ------------------------------------------------------------ raw client */

static int exchange(const char* req, int reqlen)
{
    SOCKET s;
    struct sockaddr_in sa;
    DWORD tv = 8000;
    int total = 0;
    char tmp[8192];
    int off = 0;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)g_port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) { closesocket(s); return -1; }
    while (off < reqlen) {
        int r = send(s, req + off, reqlen - off, 0);
        if (r <= 0) break;
        off += r;
    }
    for (;;) {
        int r = recv(s, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        if (total + r < (int)sizeof(g_resp) - 1) { memcpy(g_resp + total, tmp, r); total += r; }
        else break;
    }
    g_resp[total] = '\0';
    closesocket(s);
    return total;
}

static int resp_status(void)
{
    return (strncmp(g_resp, "HTTP/1.1 ", 9) == 0) ? atoi(g_resp + 9) : -1;
}

static int api_get(const char* path, const char* token)
{
    char tok[160];
    if (token) _snprintf(tok, sizeof(tok), "X-Hub-Token: %s\r\n", token);
    else tok[0] = '\0';
    _snprintf(g_req, sizeof(g_req),
              "GET %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n%sConnection: close\r\n\r\n",
              path, g_port, tok);
    return exchange(g_req, (int)strlen(g_req));
}

/* blen < 0 => strlen(body). ct NULL => omit header. send_cl 0 => omit CL. */
static int api_post(const char* path, const char* body, int blen,
                    const char* token, const char* ct, int send_cl)
{
    char tok[160], ctl[160], cll[64];
    int n;
    if (token) _snprintf(tok, sizeof(tok), "X-Hub-Token: %s\r\n", token); else tok[0] = '\0';
    if (ct)    _snprintf(ctl, sizeof(ctl), "Content-Type: %s\r\n", ct);   else ctl[0] = '\0';
    if (blen < 0) blen = body ? (int)strlen(body) : 0;
    if (send_cl) _snprintf(cll, sizeof(cll), "Content-Length: %d\r\n", blen);
    else cll[0] = '\0';
    n = _snprintf(g_req, sizeof(g_req),
                  "POST %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n%s%s%sConnection: close\r\n\r\n",
                  path, g_port, tok, ctl, cll);
    if (body && blen > 0) { memcpy(g_req + n, body, blen); n += blen; }
    return exchange(g_req, n);
}

#define POST_OK(path, body) api_post(path, body, -1, TOKEN, "application/json", 1)

static int has(const char* needle) { return strstr(g_resp, needle) != NULL; }

/* pull "key":"value" out of the last response (ticket fields etc.) */
static int grab_str(const char* key, char* out, int outsz)
{
    char pat[64];
    const char* p;
    int n = 0;
    _snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    pat[sizeof(pat) - 1] = '\0';
    p = strstr(g_resp, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (p[n] && p[n] != '"' && n < outsz - 1) { out[n] = p[n]; n++; }
    out[n] = '\0';
    return n > 0;
}

static long get_seq(void)
{
    const char* p = strstr(g_resp, "\"seq\":");
    return p ? atol(p + 6) : -1;
}

static int fp_hex_ok(void)
{
    const char* p = strstr(g_resp, "\"fingerprint\":\"");
    int i;
    if (!p) return 0;
    p += 15;
    for (i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)p[i])) return 0;
    return p[16] == '"';
}

/* ------------------------------------------------------------ pump thread */

static DWORD WINAPI pump_thread(LPVOID a)
{
    (void)a;
    while (g_pump_run) {
        if (g_pump_on) {
            while (web_api_run_pending()) { }
        }
        Sleep(5);
    }
    return 0;
}

static unsigned long g_xrs = 987654321UL;
static unsigned long xrnd(void)
{
    g_xrs ^= g_xrs << 13; g_xrs ^= g_xrs >> 17; g_xrs ^= g_xrs << 5;
    return g_xrs;
}

/* ---------------- T17 helpers: in-flight POST waiter ---------------- */
static struct {
    volatile LONG done;
    DWORD started, elapsed;
    int   status;
    char  body[1024];
} g_t17;

static DWORD WINAPI t17_client(LPVOID a)
{
    (void)a;
    g_t17.started = GetTickCount();
    api_post("/api/v1/mount", "{\"id\":\"p-ok\",\"timeoutMs\":8000}", -1,
             TOKEN, "application/json", 1);
    g_t17.elapsed = GetTickCount() - g_t17.started;
    g_t17.status = resp_status();
    strncpy(g_t17.body, g_resp, sizeof(g_t17.body) - 1);
    g_t17.body[sizeof(g_t17.body) - 1] = '\0';
    InterlockedExchange(&g_t17.done, 1);
    return 0;
}

/* ---------------- T22 helpers (R6 16-slot busy battery) ---------------- */
typedef struct { SOCKET cli; int status; char resp[1024]; HANDLE th; } T22Ctx;
static T22Ctx g_t22ctx[16];
static SOCKET g_t22srv[16];

static DWORD WINAPI t22_client(LPVOID a)
{
    int k = (int)(INT_PTR)a;
    HttpdConn conn;
    HttpdRequest rq;
    static const char body[] =
        "{\"plugin\":\"p-ok\",\"command\":\"p-ok.slow\",\"timeoutMs\":8000}";
    int n;
    conn.sock = g_t22srv[k];
    memset(&rq, 0, sizeof(rq));
    strcpy(rq.method, "POST");
    strcpy(rq.path, "/api/v1/call");
    strcpy(rq.raw_target, "/api/v1/call");
    strcpy(rq.token, TOKEN);
    strcpy(rq.content_type, "application/json");
    rq.body = (char*)body;
    rq.body_len = (int)strlen(body);
    rq.content_length = rq.body_len;
    web_api_route(&rq, &conn);
    closesocket(g_t22srv[k]);
    g_t22srv[k] = INVALID_SOCKET;
    n = recv(g_t22ctx[k].cli, g_t22ctx[k].resp, sizeof(g_t22ctx[k].resp) - 1, 0);
    if (n > 0) {
        g_t22ctx[k].resp[n] = '\0';
        g_t22ctx[k].status = atoi(g_t22ctx[k].resp + 9);
    }
    return 0;
}

/* ===================================================================== */

int main(void)
{
    WSADATA wsa;
    HANDLE pump;
    int port = 0;
    long s1, s2, s3;
    int i;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup fail\n"); return 2; }
    printf("=== creo-hub M2 web_api OFFLINE test (O4/O5/O8api/O9/O12/O13) ===\n\n");
    setup_root();
    g_hub_log_tee = fopen(g_tee_path, "wb");

    CHECK(web_api_init() == 1, "init web_api (G1 queue + events)");
    registry_scan(g_troot);
    CHECK(g_n_plugins == 3, "fixtures scanned: 3 packages");
    web_api_set_token(TOKEN);
    web_api_set_executor_ready(1);
    httpd_set_api_handler(web_api_route);
    CHECK(httpd_start(BASE_PORT, &port) == 1, "httpd start");
    g_port = port;
    pump = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
    CHECK(pump != NULL, "executor pump thread (stands in for Creo main thread)");

    /* ---------------- T1 health / G5 features (O13) ---------------- */
    printf("--- T1 health (G5/O13) ---\n");
    CHECK(api_get("/api/v1/health", TOKEN) > 0 && resp_status() == 200, "T1 health -> 200");
    CHECK(has("\"ok\":true") && has("\"apiVersion\":\"v1\""), "T1 ok:true + apiVersion v1");
#ifdef CREO_HUB_DEV
    CHECK(has("\"features\":[\"panel\",\"manifest\",\"call\",\"versioning\",\"jobs\",\"dev\"]"),
          "O13+M3b+dev features [panel,manifest,call,versioning,jobs,dev]");
#else
    CHECK(has("\"features\":[\"panel\",\"manifest\",\"call\",\"versioning\",\"jobs\"]"),
          "O13+M3b features [panel,manifest,call,versioning,jobs]");
#endif
    CHECK(has("\"executorReady\":true"), "T1 executorReady true");

    /* ---------------- T2 token gate (L3/O9) ---------------- */
    printf("--- T2 token gate (L3) ---\n");
    CHECK(api_get("/api/v1/health", NULL) > 0 && resp_status() == 401 && has("unauthorized"),
          "O9 GET health WITHOUT token -> 401");
    CHECK(api_get("/api/v1/plugins", "deadbeef") > 0 && resp_status() == 401,
          "O9 GET plugins WRONG token -> 401");
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}") > 0 && resp_status() == 200,
          "T2 right token -> through (mount p-ok)");
    CHECK(!has("Access-Control") && !has("Set-Cookie"), "O9 api responses carry no CORS/cookie headers");

    /* ---------------- T3 plugins (O4 + handle removed) ---------------- */
    printf("--- T3 plugins ---\n");
    CHECK(api_get("/api/v1/plugins", TOKEN) > 0 && resp_status() == 200, "T3 GET plugins -> 200");
    CHECK(has("\"ok\":true") && has("\"count\":3"), "T3 ok + count 3");
    CHECK(has("\"id\":\"p-ok\""), "T3 plugin ids exposed");
    CHECK(has("\"manifestVersion\":2"), "T3 manifestVersion exposed");
    CHECK(fp_hex_ok(), "R2 fingerprint exposed as 16-hex string");
    CHECK(strstr(g_resp, "handle") == NULL, "DoD: handle NOT in API surface");
    CHECK(has("\"invalidReason\"") && has("p-bad"), "T3 invalid plugin shows readable reason");
    CHECK(has("\"provides\":[\"ok.svc\"]"), "G2 provides exposed via API");
    /* p-ok was mounted in T2 -> ACTIVE now */
    CHECK(api_get("/api/v1/plugins", TOKEN) > 0 && has("\"state\":\"ACTIVE\""), "T3 p-ok ACTIVE after T2 mount");

    /* ---------------- T4 manifest (G2/L3 truth) ---------------- */
    printf("--- T4 manifest ---\n");
    CHECK(api_get("/api/v1/manifest", TOKEN) > 0 && resp_status() == 200, "T4 GET manifest -> 200");
    CHECK(has("\"id\":\"p-ok.hello\"") && has("\"title\":\"Hello\""), "T4 command id+title");
    CHECK(has("\"descForLLM\":\"Say hello; takes no arguments\""), "T4 descForLLM passthrough");
    CHECK(has("\"risk\":\"safe\""), "T4 risk badge source");
    CHECK(has("\"inputSchema\":{") && has("additionalProperties"), "T4 inputSchema embedded as JSON");
    CHECK(has("\"reversible\":-1") && has("\"timeoutSec\":0"), "T4 optional fields default visible");

    /* ---------------- T5 log endpoint ---------------- */
    printf("--- T5 log tail ---\n");
    CHECK(api_get("/api/v1/log?offset=0", TOKEN) > 0 && resp_status() == 200, "T5 log offset=0 -> 200");
    CHECK(has("M2-LOGTEST-1") && has("M2-LOGTEST-3"), "T5 seeded lines returned");
    {
        const char* p = strstr(g_resp, "\"offset\":");
        long off1 = p ? atol(p + 9) : -1;
        char path[64];
        CHECK(off1 > 0, "T5 offset advanced (>0)");
        _snprintf(path, sizeof(path), "/api/v1/log?offset=%ld", off1);
        CHECK(api_get(path, TOKEN) > 0 && resp_status() == 200 && has("\"lines\":[]"),
              "T5 second poll at EOF -> empty lines");
        _snprintf(path, sizeof(path), "/api/v1/log?offset=%ld", off1);
        /* append a line, poll again -> exactly the new line */
        {
            char lp[MAX_PATH + 48];
            FILE* lf;
            _snprintf(lp, sizeof(lp), "%s\\logs\\hub_m1.log", g_troot);
            lf = fopen(lp, "ab");
            if (lf) { fputs("[t3] [API] M2-LOGTEST-4\n", lf); fclose(lf); }
        }
        CHECK(api_get(path, TOKEN) > 0 && has("M2-LOGTEST-4") && !has("M2-LOGTEST-1"),
              "T5 incremental tail: only NEW line returned");
    }
    CHECK(api_get("/api/v1/log?offset=abc", TOKEN) > 0 && resp_status() == 400, "T5 offset=abc -> 400");
    CHECK(api_get("/api/v1/log?offset=99999999", TOKEN) > 0 && resp_status() == 200 &&
          has("M2-LOGTEST-1"), "T5 offset past EOF -> resets to 0");

    /* ---------------- T6 unknown endpoints (L8) ---------------- */
    printf("--- T6 route table ---\n");
    CHECK(api_get("/api/v1/nope", TOKEN) > 0 && resp_status() == 404, "T6 GET unknown -> 404");
    CHECK(POST_OK("/api/v1/nope", "{}") > 0 && resp_status() == 404, "T6 POST unknown -> 404");

    /* ---------------- T7 POST discipline (L4) ---------------- */
    printf("--- T7 L4 JSON enforcement ---\n");
    CHECK(api_post("/api/v1/mount", "{\"id\":\"p-ok\"}", -1, TOKEN, "text/plain", 1) > 0 &&
          resp_status() == 415, "O9 POST text/plain -> 415");
    CHECK(api_post("/api/v1/mount", "{\"id\":\"p-ok\"}", -1, TOKEN, NULL, 1) > 0 &&
          resp_status() == 415, "O9 POST without Content-Type -> 415");
    CHECK(api_post("/api/v1/mount", "{\"id\":\"p-ok\"}", -1, TOKEN, "application/json", 0) > 0 &&
          resp_status() == 411, "O9 POST without Content-Length -> 411");
    CHECK(POST_OK("/api/v1/mount", "{[") > 0 && resp_status() == 400, "O9 malformed JSON -> 400");
    CHECK(POST_OK("/api/v1/mount", "") > 0 && resp_status() == 400, "T7 empty body -> 400");
    CHECK(POST_OK("/api/v1/mount", "{}") > 0 && resp_status() == 200 &&
          has("\"ok\":false") && has("id required"), "T7 missing id -> 200 ok:false");
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"ghost\"}") > 0 && resp_status() == 200 &&
          has("not found"), "T7 unknown plugin -> 200 ok:false not-found");
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\",\"timeoutMs\":\"x\"}") > 0 &&
          resp_status() == 400, "T7 bad timeoutMs -> 400");

    /* ---------------- T8 mount/unmount lifecycle (O4) ---------------- */
    printf("--- T8 lifecycle ---\n");
    CHECK(POST_OK("/api/v1/unmount", "{\"id\":\"p-ok\"}") > 0 && resp_status() == 200 &&
          has("\"ok\":true") && has("\"state\":\"REGISTERED\""), "T8 unmount p-ok -> REGISTERED");
    s1 = get_seq();
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}") > 0 && has("\"ok\":true") &&
          has("\"state\":\"ACTIVE\""), "T8 mount p-ok -> ACTIVE");
    s2 = get_seq();
    CHECK(POST_OK("/api/v1/unmount", "{\"id\":\"p-ok\"}") > 0 && has("\"ok\":true"), "T8 unmount again");
    s3 = get_seq();
    CHECK(s1 >= 0 && s2 > s1 && s3 > s2, "G1 seq strictly increasing across envelopes");
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}") > 0 && has("\"ok\":true"), "T8 remount");
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}") > 0 && resp_status() == 200 &&
          has("\"state\":\"ACTIVE\""), "T8 mount while ACTIVE -> idempotent, still ACTIVE");
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-fail\"}") > 0 && resp_status() == 200 &&
          has("\"ok\":false") && has("GENERAL_ERROR") && has("\"state\":\"ERROR\""),
          "O4 business failure = HTTP 200 + ok:false + code + state");
    CHECK(POST_OK("/api/v1/unmount", "{\"id\":\"p-bad\"}") > 0 && resp_status() == 200 &&
          has("\"ok\":false"), "T8 unmount INVALID -> 200 ok:false (guard holds)");

    /* ---------------- T9 automount + persistence ---------------- */
    printf("--- T9 automount ---\n");
    CHECK(POST_OK("/api/v1/automount", "{\"id\":\"p-ok\",\"value\":true}") > 0 &&
          resp_status() == 200 && has("\"ok\":true") && has("\"autoMount\":true"),
          "T9 automount on -> ok");
    CHECK(api_get("/api/v1/plugins", TOKEN) > 0 && has("\"autoMount\":true"), "T9 plugins reflects autoMount");
    {
        char stp[MAX_PATH + 48];
        char* stx;
        _snprintf(stp, sizeof(stp), "%s\\hub_state.json", g_troot);
        stx = read_file_alloc(stp);
        CHECK(stx && strstr(stx, "autoMount") && strstr(stx, "p-ok"),
              "T9 hub_state.json persisted autoMount=true");
        free(stx);
    }
    CHECK(POST_OK("/api/v1/automount", "{\"id\":\"p-ok\",\"value\":\"yes\"}") > 0 &&
          resp_status() == 200 && has("\"ok\":false"), "T9 automount non-bool -> 200 ok:false");

    /* ---------------- T10 refresh ---------------- */
    printf("--- T10 refresh ---\n");
    CHECK(POST_OK("/api/v1/refresh", "{}") > 0 && resp_status() == 200 && has("\"ok\":true") &&
          has("\"total\":3"), "T10 refresh -> totals");

    /* ---------------- T11 bulk reconcile ops (G3) ---------------- */
    printf("--- T11 mount-all / unload-all ---\n");
    CHECK(POST_OK("/api/v1/unmount", "{\"id\":\"p-ok\"}") > 0 && has("\"ok\":true"), "T11 pre: p-ok down");
    CHECK(POST_OK("/api/v1/mount-all", "{}") > 0 && resp_status() == 200 && has("\"ok\":true") &&
          has("\"mounted\":1") && has("\"failed\":1"),
          "T11 mount-all: p-ok mounted, p-fail failed (ERROR retried)");
    CHECK(api_get("/api/v1/plugins", TOKEN) > 0 && has("\"state\":\"ACTIVE\""), "T11 p-ok ACTIVE after mount-all");
    CHECK(POST_OK("/api/v1/unload-all", "{}") > 0 && has("\"ok\":true") && has("\"unmounted\":1"),
          "T11 unload-all: 1 unmounted");
    CHECK(api_get("/api/v1/plugins", TOKEN) > 0 && !has("\"state\":\"ACTIVE\""),
          "T11 nothing ACTIVE after unload-all");

    /* ---------------- T12 executor not ready ---------------- */
    printf("--- T12 executor readiness ---\n");
    web_api_set_executor_ready(0);
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}") > 0 && resp_status() == 503 &&
          has("hub not ready"), "O5 executor not ready -> 503 immediately");
    web_api_set_executor_ready(1);

    /* ---------------- T13 timeout + abandon (G1) ---------------- */
    printf("--- T13 timeout path ---\n");
    InterlockedExchange(&g_pump_on, 0);
    Sleep(60);
    CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\",\"timeoutMs\":200}") > 0 &&
          resp_status() == 200 && has("\"ok\":false") && has("\"error\":\"timeout\""),
          "G1 executor stalled -> 200 ok:false error=timeout (not a hang)");
    CHECK(web_api_queue_used() == 1, "G1 abandoned envelope still occupies its slot");

    /* ---------------- T14 queue full -> 503 busy (O5) ---------------- */
    printf("--- T14 queue full (takes ~2.5s) ---\n");
    {
        int saw503 = 0, timeouts = 0;
        /* T13 left 1 abandoned slot; 16 more attempts => the 17th envelope
         * overall must hit the full queue (Q_SLOTS=16) with an instant 503. */
        for (i = 0; i < 16; i++) {
            api_post("/api/v1/mount", "{\"id\":\"p-ok\",\"timeoutMs\":120}", -1,
                     TOKEN, "application/json", 1);
            if (resp_status() == 503 && has("busy")) saw503 = 1;
            else if (resp_status() == 200 && has("timeout")) timeouts++;
        }
        CHECK(saw503 == 1, "O5 17th pending envelope -> 503 busy (queue full)");
        CHECK(timeouts >= 15, "O5 preceding envelopes each timed out cleanly");
        InterlockedExchange(&g_pump_on, 1);
        {
            int waited = 0;
            while (web_api_queue_used() > 0 && waited < 4000) { Sleep(50); waited += 50; }
        }
        CHECK(web_api_queue_used() == 0, "G1 executor drains abandoned slots (drop-without-execute)");
        CHECK(POST_OK("/api/v1/mount", "{\"id\":\"p-ok\",\"timeoutMs\":3000}") > 0 &&
              has("\"ok\":true"), "G1 queue healthy again after drain");
        CHECK(POST_OK("/api/v1/unmount", "{\"id\":\"p-ok\"}") > 0 && has("\"ok\":true"), "T14 cleanup unmount");
    }

    /* ---------------- T15 audit channel (R3/O12) ---------------- */
    printf("--- T15 audit (R3) ---\n");
    {
        char* tee = read_file_alloc(g_tee_path);
        CHECK(tee != NULL, "O12 audit tee file written");
        if (tee) {
            CHECK(strstr(tee, "[AUDIT]") != NULL, "R3 [AUDIT] lines present");
            CHECK(strstr(tee, "op=mount plugin=p-ok") != NULL, "R3 op+plugin fields");
            CHECK(strstr(tee, "args={\"id\":\"p-ok\"}") != NULL, "R3 args field carries request JSON");
            CHECK(strstr(tee, "result=ACTIVE err=NO_ERROR") != NULL, "R3 result field (state + err)");
            CHECK(strstr(tee, "ms=") != NULL, "R3 duration field");
            CHECK(strstr(tee, "REJECT(unauthorized)") != NULL, "R3 401 rejections audited");
            CHECK(strstr(tee, "REJECT(content-type)") != NULL, "R3 415 rejections audited");
            CHECK(strstr(tee, "result=TIMEOUT") != NULL, "R3 timeouts audited");
            CHECK(strstr(tee, "DROPPED(abandoned") != NULL, "R3 abandoned drops audited");
            free(tee);
        }
    }

    /* ---------------- T16 O8 api-layer fuzz ---------------- */
    printf("--- T16 O8 api fuzz x300 ---\n");
    {
        int bad = 0, dead = 0;
        for (i = 0; i < 300; i++) {
            unsigned long mode = xrnd() % 5;
            int st;
            if (mode == 0) {          /* random bytes as JSON body */
                char body[256];
                int blen = (int)(xrnd() % 200);
                int k;
                for (k = 0; k < blen; k++) body[k] = (char)(xrnd() & 0xFF);
                api_post("/api/v1/mount", body, blen, TOKEN, "application/json", 1);
            } else if (mode == 1) {   /* half-valid JSON */
                char body[128];
                _snprintf(body, sizeof(body), "{\"id\":%lu}", xrnd());
                POST_OK("/api/v1/mount", body);
            } else if (mode == 2) {   /* valid JSON, wrong root type */
                static const char* types[5] = { "[]", "\"str\"", "123", "null", "true" };
                POST_OK("/api/v1/automount", types[xrnd() % 5]);
            } else if (mode == 3) {   /* junk query on log */
                char path[128];
                _snprintf(path, sizeof(path), "/api/v1/log?offset=%lu%%zz", xrnd() % 1000);
                api_get(path, TOKEN);
            } else {                  /* junk GET path under /api/v1 */
                char path[128];
                int k, n = sprintf(path, "/api/v1/");
                for (k = 0; k < 12; k++) path[n++] = (char)('a' + xrnd() % 26);
                path[n] = '\0';
                api_get(path, TOKEN);
            }
            st = resp_status();
            if (st == -1 && g_resp[0] == '\0') { dead++; continue; }   /* no response at all */
            if (st >= 500 && st != 503) {
                bad++;
                printf("  fuzz[%d] mode=%lu 5xx status=%d\n", i, mode, st);
            }
        }
        CHECK(dead == 0, "O8 every fuzz request got a response (no silent drops)");
        CHECK(bad == 0, "O8 no 5xx from api fuzz (503-busy excluded, none expected here)");
        CHECK(api_get("/api/v1/health", TOKEN) > 0 && resp_status() == 200 && has("\"ok\":true"),
              "O8 health still 200 after fuzz");
        CHECK(api_get("/api/v1/plugins", TOKEN) > 0 && has("\"count\":3"),
              "O8 registry intact after fuzz");
        CHECK(httpd_is_running() == 1, "O8 server alive after fuzz");
    }

    /* ---------------- T18 M3b b2: /call + nonce + confirm + orphan ---------------- */
    printf("--- T18 /call nonce + orphan protocol (M3b b2) ---\n");
    {
        char body[640], txn[64], nonce[64];
        char* tee;
        POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}");   /* earlier tests may leave it unmounted */
        CHECK(resp_status() == 200 && has("\"state\":\"ACTIVE\""), "T18 p-ok ACTIVE for call tests");
        strcpy(g_stub_out_json, "{\"ok\":true,\"result\":{\"hello\":\"world\"}}");

        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.hello\"}");
        CHECK(resp_status() == 200 && has("\"ok\":true") && has("\"hello\":\"world\"")
              && has("\"state\""), "T18 safe call -> 200 {ok,result,state}");
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.nope\"}");
        CHECK(resp_status() == 404, "T18 unknown command -> 404");
        POST_OK("/api/v1/call", "{\"plugin\":\"nope\",\"command\":\"p-ok.hello\"}");
        CHECK(resp_status() == 404 && has("plugin not found"), "T18 unknown plugin -> 404");
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.hello\",\"dryRun\":true}");
        CHECK(resp_status() == 200 && has("\"dryRun\":true") && has("wouldExecute")
              && has("argsDigest"), "T18 dryRun -> 200 wouldExecute (+argsDigest)");

        /* nonce flow over HTTP (bridge-proxied approval loop) */
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\"}");
        CHECK(resp_status() == 428 && has("needConfirm") && has("\"nonce\"")
              && grab_str("transactionId", txn, sizeof(txn))
              && grab_str("nonce", nonce, sizeof(nonce)),
              "T18 confirm-level -> 428 + ticket");
        _snprintf(body, sizeof(body), "{\"transactionId\":\"%s\",\"approve\":true}", txn);
        POST_OK("/api/v1/confirm", body);
        CHECK(resp_status() == 200 && has("APPROVED"), "T18 POST /confirm approve -> 200 APPROVED");
        POST_OK("/api/v1/confirm", "{\"transactionId\":\"zz\",\"approve\":true}");
        CHECK(resp_status() == 404, "T18 confirm unknown txn -> 404");
        POST_OK("/api/v1/confirm", "{\"approve\":true}");
        CHECK(resp_status() == 400, "T18 confirm missing txn -> 400");
        _snprintf(body, sizeof(body),
                  "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\",\"confirm_nonce\":\"%s\"}", nonce);
        POST_OK("/api/v1/call", body);
        CHECK(resp_status() == 200 && has("\"ok\":true"),
              "T18 retry WITH approved nonce -> 200 executed");
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\"}");
        CHECK(resp_status() == 428, "T18 consumed: next call needs fresh confirmation");

        /* auto-pair path (approved in panel, client retries WITHOUT nonce) */
        CHECK(grab_str("transactionId", txn, sizeof(txn)), "T18 fresh ticket for auto-pair");
        _snprintf(body, sizeof(body), "{\"transactionId\":\"%s\",\"approve\":true}", txn);
        POST_OK("/api/v1/confirm", body);
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\"}");
        CHECK(resp_status() == 200 && has("\"ok\":true"),
              "T18 auto-pair: retry WITHOUT nonce -> 200");

        /* deny path */
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\"}");
        CHECK(resp_status() == 428 && grab_str("transactionId", txn, sizeof(txn)),
              "T18 ticket for deny");
        _snprintf(body, sizeof(body), "{\"transactionId\":\"%s\",\"approve\":false}", txn);
        POST_OK("/api/v1/confirm", body);
        CHECK(resp_status() == 200 && has("DENIED"), "T18 confirm deny -> 200 DENIED");
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\"}");
        CHECK(resp_status() == 428, "T18 after deny: fresh 428 (old nonce dead)");

        /* M3e Q1 operator mode menu (route + arg discipline + health feed) */
        POST_OK("/api/v1/opmode", "{\"mode\":1}");
        CHECK(resp_status() == 200 && has("\"ok\":true") && has("\"mode\":1"),
              "T19 arm M1 -> 200 mode=1");
        api_get("/api/v1/health", TOKEN);
        CHECK(resp_status() == 200 && has("\"opmode\":1"),
              "T19 health carries opmode=1 (panel banner feed)");
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\"}");
        CHECK(resp_status() == 200 && has("\"ok\":true"),
              "T19 M1: confirm-level call executes without 428");
        POST_OK("/api/v1/opmode", "{\"mode\":2,\"minutes\":10}");
        CHECK(resp_status() == 400 && has("ack:true"),
              "T19 M2 without ack -> 400 (explicit consent mandatory)");
        POST_OK("/api/v1/opmode", "{\"mode\":2,\"minutes\":7,\"ack\":true}");
        CHECK(resp_status() == 400 && has("{5,10,30}"),
              "T19 M2 non-whitelisted minutes -> 400");
        POST_OK("/api/v1/opmode", "{\"mode\":2,\"minutes\":5,\"ack\":true}");
        CHECK(resp_status() == 200 && has("\"mode\":2") && has("\"remainMs\""),
              "T19 arm M2 5min ack -> 200 with remainMs");
        POST_OK("/api/v1/opmode", "{\"mode\":9}");
        CHECK(resp_status() == 400 && has("mode must be 0, 1 or 2"),
              "T19 bogus mode -> 400");
        POST_OK("/api/v1/opmode", "{\"mode\":0}");
        CHECK(resp_status() == 200 && has("\"mode\":0"), "T19 disarm -> M0");
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.risky\"}");
        CHECK(resp_status() == 428, "T19 M0 restored: confirm 428 again");

        /* orphan protocol: started + envelope timeout -> 504 report-not-kill */
        g_stub_sleep_ms = 3000;
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.slow\"}");
        CHECK(resp_status() == 504 && has("deadline exceeded") && has("\"timedOut\":true")
              && has("\"retryable\":true") && has("may still be running"),
              "T18 started+timeout -> 504 orphan semantics (retryable: idempotent)");
        api_get("/api/v1/health", TOKEN);
        CHECK(resp_status() == 200 && has("\"executorReady\":false") && has("\"orphaned\":true"),
              "T18 orphan window: executorReady=false + orphaned=true");
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.hello\"}");
        CHECK(resp_status() == 503 && has("busy-orphaned"),
              "T18 new /call during orphan -> 503 busy-orphaned");
        for (i = 0; i < 50; i++) {
            api_get("/api/v1/health", TOKEN);
            if (has("\"orphaned\":false")) break;
            Sleep(100);
        }
        CHECK(has("\"orphaned\":false") && has("\"executorReady\":true"),
              "T18 late completion clears orphan window");
        g_stub_sleep_ms = 0;
        tee = read_file_alloc(g_tee_path);
        CHECK(tee && strstr(tee, "op=call.late") != NULL, "T18 op=call.late audited in tee");
        /* ISSUE-L1 regression: a client-presented confirm_nonce must land in
         * the audit log MASKED (first 8 hex + '*'), never in full. */
        CHECK(tee && strstr(tee, "\"confirm_nonce\":\"") != NULL,
              "T18 confirm_nonce appears in audited args (setup for mask check)");
        CHECK(tee && strstr(tee, nonce) == NULL,
              "T18 full nonce NEVER in audit log (ISSUE-L1 mask)");
        {
            char masked[16];
            strncpy(masked, nonce, 8); masked[8] = '*'; masked[9] = '\0';
            CHECK(tee && strstr(tee, masked) != NULL, "T18 masked form nonce_id+* present");
        }
        free(tee);

        /* cancelled-before-start: pump frozen -> envelope timeout, item QUEUED */
        g_pump_on = 0;
        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.hello\",\"timeoutMs\":300}");
        CHECK(resp_status() == 504 && has("cancelledBeforeStart") && has("\"retryable\":true")
              && has("never executed"), "T18 queued timeout -> 504 cancelled-before-start");
        api_get("/api/v1/health", TOKEN);
        CHECK(has("\"orphaned\":false"), "T18 cancelled-before-start does NOT orphan");
        g_pump_on = 1;
        Sleep(200);
        CHECK(web_api_queue_used() == 0, "T18 pump dropped the cancelled envelope");
    }

    /* ---------------- T19 M3b b3: drain-unmount plan + cascade ---------------- */
    printf("--- T19 drain-unmount dryRun + cascade (M3b b3) ---\n");
    {
        mkpkg("p-dep", "p-dep.dll",
              "{\"manifestVersion\":2,\"id\":\"p-dep\",\"name\":\"Dep pkg\",\"version\":\"1.0.0\","
              "\"adapter\":\"ptk-sync\",\"entry\":\"bin/p-dep.dll\",\"textDir\":\"text\","
              "\"requires\":[\"p-ok\"],"
              "\"commands\":[{\"id\":\"p-dep.one\",\"title\":\"One\",\"desc\":\"d\","
              "\"descForLLM\":\"d\",\"risk\":\"safe\","
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}");
        POST_OK("/api/v1/refresh", "{}");
        CHECK(resp_status() == 200, "T19 refresh picks up p-dep");
        POST_OK("/api/v1/mount", "{\"id\":\"p-dep\"}");
        CHECK(resp_status() == 200 && has("\"state\":\"ACTIVE\""),
              "T19 p-dep mounts while provider p-ok ACTIVE");
        POST_OK("/api/v1/unmount", "{\"id\":\"p-ok\",\"dryRun\":true}");
        CHECK(resp_status() == 200 && has("\"dryRun\":true") &&
              has("\"drainedList\":[\"p-dep\",\"p-ok\"]"),
              "T19 dryRun returns reverse-topo plan [dependent, target]");
        api_get("/api/v1/plugins", TOKEN);
        CHECK(resp_status() == 200 && has("\"state\":\"ACTIVE\""),
              "T19 dryRun executed nothing (p-ok still ACTIVE)");
        POST_OK("/api/v1/unmount", "{\"id\":\"p-ok\"}");
        CHECK(resp_status() == 200 && has("\"ok\":true") &&
              has("\"drainedList\":[\"p-dep\",\"p-ok\"]"),
              "T19 real unmount cascades dependent first, reports drainedList");
        api_get("/api/v1/plugins", TOKEN);
        CHECK(resp_status() == 200 && !has("\"state\":\"ACTIVE\""),
              "T19 after drain both records REGISTERED");
        POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}");
        CHECK(resp_status() == 200 && has("\"state\":\"ACTIVE\""),
              "T19 provider remounts alone (no cascade mount)");
        POST_OK("/api/v1/unmount", "{\"id\":\"p-ok\"}");
        POST_OK("/api/v1/unmount", "{\"id\":\"p-dep\"}");
    }

    /* ---------------- T20 M3b b3: P1 update/rollback verbs ---------------- */
    printf("--- T20 P1 version pointer update/rollback (M3b b3) ---\n");
    {
        char d[MAX_PATH], f[MAX_PATH];
        _snprintf(d, sizeof(d), "%s\\plugins\\p-ver", g_troot); mkdir_p(d);
        _snprintf(f, sizeof(f), "%s\\plugin.json", d);
        write_file(f, "{\"manifestVersion\":2,\"id\":\"p-ver\",\"adapter\":\"ptk-sync\","
                      "\"currentVersion\":\"1.0.0\"}");
        _snprintf(d, sizeof(d), "%s\\plugins\\p-ver\\versions\\1.0.0\\bin", g_troot); mkdir_p(d);
        _snprintf(d, sizeof(d), "%s\\plugins\\p-ver\\versions\\1.0.0\\text\\text\\usascii", g_troot); mkdir_p(d);
        _snprintf(f, sizeof(f), "%s\\plugins\\p-ver\\versions\\1.0.0\\bin\\p-ver.dll", g_troot);
        write_file(f, "MZ-fake");
        _snprintf(f, sizeof(f), "%s\\plugins\\p-ver\\versions\\1.0.0\\text\\text\\usascii\\p-ver_messages.txt", g_troot);
        write_file(f, "#MSG dummy\n");
        _snprintf(f, sizeof(f), "%s\\plugins\\p-ver\\versions\\1.0.0\\plugin.json", g_troot);
        write_file(f, "{\"manifestVersion\":2,\"id\":\"p-ver\",\"name\":\"ver\","
                      "\"version\":\"1.0.0\",\"adapter\":\"ptk-sync\","
                      "\"entry\":\"bin/p-ver.dll\",\"textDir\":\"text\","
                      "\"commands\":[{\"id\":\"p-ver.one\",\"title\":\"t\",\"desc\":\"d\","
                      "\"descForLLM\":\"d\",\"risk\":\"safe\","
                      "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}");
        _snprintf(d, sizeof(d), "%s\\plugins\\p-ver\\versions\\1.1.0\\bin", g_troot); mkdir_p(d);
        _snprintf(d, sizeof(d), "%s\\plugins\\p-ver\\versions\\1.1.0\\text\\text\\usascii", g_troot); mkdir_p(d);
        _snprintf(f, sizeof(f), "%s\\plugins\\p-ver\\versions\\1.1.0\\bin\\p-ver.dll", g_troot);
        write_file(f, "MZ-fake");
        _snprintf(f, sizeof(f), "%s\\plugins\\p-ver\\versions\\1.1.0\\text\\text\\usascii\\p-ver_messages.txt", g_troot);
        write_file(f, "#MSG dummy\n");
        _snprintf(f, sizeof(f), "%s\\plugins\\p-ver\\versions\\1.1.0\\plugin.json", g_troot);
        write_file(f, "{\"manifestVersion\":2,\"id\":\"p-ver\",\"name\":\"ver\","
                      "\"version\":\"1.1.0\",\"adapter\":\"ptk-sync\","
                      "\"entry\":\"bin/p-ver.dll\",\"textDir\":\"text\","
                      "\"commands\":[{\"id\":\"p-ver.eleven\",\"title\":\"t\",\"desc\":\"d\","
                      "\"descForLLM\":\"d\",\"risk\":\"safe\","
                      "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}");
        POST_OK("/api/v1/refresh", "{}");
        api_get("/api/v1/plugins", TOKEN);
        CHECK(resp_status() == 200 && has("\"currentVersion\":\"1.0.0\"") &&
              has("p-ver.one"), "T20 pointer package scanned at 1.0.0");
        POST_OK("/api/v1/update", "{\"id\":\"p-ver\",\"version\":\"1.1.0\"}");
        CHECK(resp_status() == 200 && has("\"ok\":true") &&
              has("\"currentVersion\":\"1.1.0\""), "T20 update switches pointer");
        api_get("/api/v1/plugins", TOKEN);
        CHECK(resp_status() == 200 && has("p-ver.eleven"),
              "T20 effective commands flipped to 1.1.0");
        POST_OK("/api/v1/mount", "{\"id\":\"p-ver\"}");
        CHECK(resp_status() == 200 && has("\"state\":\"ACTIVE\""), "T20 p-ver ACTIVE at 1.1.0");
        POST_OK("/api/v1/update", "{\"id\":\"p-ver\",\"version\":\"1.0.0\"}");
        CHECK(resp_status() == 200 && has("\"ok\":true") && has("\"state\":\"ACTIVE\""),
              "T20 update while ACTIVE remounts (session-internal HMR)");
        api_get("/api/v1/plugins", TOKEN);
        CHECK(resp_status() == 200 && has("p-ver.one"), "T20 ACTIVE record now runs 1.0.0");
        POST_OK("/api/v1/update", "{\"id\":\"p-ver\",\"version\":\"7.7.7\"}");
        CHECK(resp_status() == 200 && has("\"ok\":false") && has("version dir missing"),
              "T20 update to absent version dir refused");
        POST_OK("/api/v1/rollback", "{\"id\":\"p-ver\",\"version\":\"1.1.0\"}");
        CHECK(resp_status() == 200 && has("\"ok\":true") && has("\"state\":\"ACTIVE\""),
              "T20 rollback verb restores 1.1.0 while ACTIVE");
        POST_OK("/api/v1/unmount", "{\"id\":\"p-ver\"}");
    }

    /* ---------------- T22 M3b b3 R6: 16-slot 503-busy battery ---------------- */
    printf("--- T22 R6 queue-full 503-busy battery (direct-route injection) ---\n");
    POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}");
    {
        /* httpd is a serial accept loop (live ruling L-M3b-1 #1): 16 concurrent
         * envelopes are architecturally unreachable over the wire. R6 therefore
         * injects web_api_route directly from 16 threads over private socket
         * pairs - the queue/503 contract is the unit under test. */
        int k, filled = 0, ok200 = 0;

        for (k = 0; k < 16; k++) {
            struct sockaddr_in la;
            SOCKET ls;
            int lan = sizeof(la);
            ls = socket(AF_INET, SOCK_STREAM, 0);
            memset(&la, 0, sizeof(la));
            la.sin_family = AF_INET;
            la.sin_addr.s_addr = inet_addr("127.0.0.1");
            la.sin_port = 0;
            bind(ls, (struct sockaddr*)&la, sizeof(la));
            getsockname(ls, (struct sockaddr*)&la, &lan);
            listen(ls, 1);
            g_t22ctx[k].cli = socket(AF_INET, SOCK_STREAM, 0);
            connect(g_t22ctx[k].cli, (struct sockaddr*)&la, sizeof(la));
            g_t22srv[k] = accept(ls, NULL, NULL);
            closesocket(ls);
            g_t22ctx[k].status = 0; g_t22ctx[k].resp[0] = '\0';
        }
        g_pump_on = 0;                                /* freeze executor */
        for (k = 0; k < 16; k++) {
            g_t22ctx[k].th = CreateThread(NULL, 0, t22_client, (LPVOID)(INT_PTR)k, 0, NULL);
            if (g_t22ctx[k].th) filled++;
        }
        CHECK(filled == 16, "T22 16 route-injection threads started");
        for (k = 0; k < 60; k++) {                    /* wait for queue saturation */
            if (web_api_queue_used() == 16) break;
            Sleep(50);
        }
        CHECK(web_api_queue_used() == 16, "T22 queue holds 16 envelopes (frozen pump)");
        {   /* 17th call on its own pair -> fast 503 busy, run inline */
            T22Ctx c17;
            struct sockaddr_in la;
            SOCKET ls;
            int lan = sizeof(la);
            ls = socket(AF_INET, SOCK_STREAM, 0);
            memset(&la, 0, sizeof(la));
            la.sin_family = AF_INET;
            la.sin_addr.s_addr = inet_addr("127.0.0.1");
            la.sin_port = 0;
            bind(ls, (struct sockaddr*)&la, sizeof(la));
            getsockname(ls, (struct sockaddr*)&la, &lan);
            listen(ls, 1);
            c17.cli = socket(AF_INET, SOCK_STREAM, 0);
            connect(c17.cli, (struct sockaddr*)&la, sizeof(la));
            {
                SOCKET s17 = accept(ls, NULL, NULL);
                HttpdConn conn17; HttpdRequest rq17;
                static char body17[] = "{\"plugin\":\"p-ok\",\"command\":\"p-ok.slow\",\"timeoutMs\":8000}";
                closesocket(ls);
                conn17.sock = s17;
                memset(&rq17, 0, sizeof(rq17));
                strcpy(rq17.method, "POST");
                strcpy(rq17.path, "/api/v1/call");
                strcpy(rq17.raw_target, "/api/v1/call");
                strcpy(rq17.token, TOKEN);
                strcpy(rq17.content_type, "application/json");
                rq17.body = body17;
                rq17.body_len = (int)strlen(body17);
                rq17.content_length = rq17.body_len;
                web_api_route(&rq17, &conn17);
                closesocket(s17);
            }
            c17.status = 0; c17.resp[0] = '\0';
            {
                int n = recv(c17.cli, c17.resp, sizeof(c17.resp) - 1, 0);
                if (n > 0) { c17.resp[n] = '\0'; c17.status = atoi(c17.resp + 9); }
            }
            closesocket(c17.cli);
            CHECK(c17.status == 503 && strstr(c17.resp, "\"error\":\"busy\"") &&
                  strstr(c17.resp, "\"retryAfterMs\":500"),
                  "T22 17th concurrent call -> 503 busy + retryAfterMs 500");
        }
        api_get("/api/v1/health", TOKEN);
        CHECK(resp_status() == 200 && has("\"queueUsed\":16") && has("\"orphaned\":false"),
              "T22 health reports queueUsed=16, no orphan (queue-full is not an orphan)");
        g_pump_on = 1;                                /* drain: all 16 execute */
        for (k = 0; k < 16; k++) {
            if (g_t22ctx[k].th) {
                WaitForSingleObject(g_t22ctx[k].th, 15000);
                CloseHandle(g_t22ctx[k].th);
            }
            if (g_t22ctx[k].status == 200 && strstr(g_t22ctx[k].resp, "\"ok\":true")) ok200++;
            closesocket(g_t22ctx[k].cli);
            if (g_t22srv[k] != INVALID_SOCKET) closesocket(g_t22srv[k]);
        }
        CHECK(ok200 == 16, "T22 all 16 frozen-envelope calls complete 200 after unfreeze");
        CHECK(web_api_queue_used() == 0, "T22 queue drained to 0");
        g_stub_sleep_ms = 0;
    }

#ifdef CREO_HUB_DEV
    /* ------------- T23 R8: dev-face regression (E11 endpoint, dev builds only) ------------- */
    printf("--- T23 R8 dev-face _e11 regression (CREO_HUB_DEV) ---\n");
    POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}");
    POST_OK("/api/v1/_e11", "{\"mode\":\"mem\"}");
    CHECK(resp_status() == 200 && has("\"mode\":\"mem\"") && has("mem_kb"),
          "T23 _e11 mem sampling -> 200 with mem_kb (E11-10)");
    POST_OK("/api/v1/_e11",
            "{\"plugin\":\"p-ok\",\"route\":\"B\",\"func\":\"hub_cmd\","
            "\"in_json\":\"{\\\"command\\\":\\\"p-ok.hello\\\"}\"}");
    CHECK(resp_status() == 200 && has("\"ok\":true") && has("\"channel\":\"OK\"") &&
          has("\"route\":\"B\""),
          "T23 _e11 route B on ACTIVE plugin -> channel OK (stub)");
    POST_OK("/api/v1/_e11", "{\"plugin\":\"p-ok\",\"route\":\"A\",\"func\":\"hub_cmd\"}");
    CHECK(resp_status() == 200 && has("\"route\":\"A\"") && has("\"channel\":\"OK\""),
          "T23 _e11 route A shares the promoted adapter_call seam (E11-5)");
    POST_OK("/api/v1/_e11", "{\"plugin\":\"no-such\"}");
    CHECK(resp_status() == 404 && has("plugin not found"),
          "T23 _e11 unknown plugin -> 404");
    POST_OK("/api/v1/_e11", "{\"plugin\":\"p-fail\"}");
    CHECK(resp_status() == 409 && has("plugin not active"),
          "T23 _e11 non-ACTIVE plugin -> 409 (E11-6 state gate precedes channel)");
    api_get("/api/v1/health", TOKEN);
    CHECK(resp_status() == 200 && has("\"dev\""), "T23 health features carry dev");
#endif

    /* ---------------- T24 b4 layer 10: Origin whitelist (docs\23 3.8 ruling 2026-10-10:
       absent -> pass; loopback http(scheme) with matching port -> pass; all else 403) ---- */
    printf("--- T24 Origin whitelist (b4 L10) ---\n");
    {
        char rq[1024], org[64];
        _snprintf(org, sizeof(org), "http://127.0.0.1:%d", g_port);
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: %s\r\nConnection: close\r\n\r\n", g_port, TOKEN, org);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 200,
              "T24 loopback Origin (panel POST shape) -> 200");
        _snprintf(org, sizeof(org), "http://localhost:%d", g_port);
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: localhost:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: %s\r\nConnection: close\r\n\r\n", g_port, TOKEN, org);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 200,
              "T24 localhost Origin -> 200");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: http://127.0.0.1\r\nConnection: close\r\n\r\n", g_port, TOKEN);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 200,
              "T24 loopback Origin without port -> 200");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: http://evil.com\r\nConnection: close\r\n\r\n", g_port, TOKEN);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 403,
              "T24 external Origin -> 403");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: http://127.0.0.1:9999\r\nConnection: close\r\n\r\n", g_port, TOKEN);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 403,
              "T24 wrong-port Origin -> 403");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: null\r\nConnection: close\r\n\r\n", g_port, TOKEN);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 403,
              "T24 sandboxed 'null' Origin -> 403");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: https://127.0.0.1:%d\r\nConnection: close\r\n\r\n", g_port, TOKEN, g_port);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 403,
              "T24 https scheme Origin -> 403");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: http://127.0.0.1:%d.evil.com\r\nConnection: close\r\n\r\n",
                  g_port, TOKEN, g_port);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 403,
              "T24 loopback-prefixed host suffix trick -> 403");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: http://evil.com\r\nOrigin: http://127.0.0.1:%d\r\n"
                  "Connection: close\r\n\r\n", g_port, TOKEN, g_port);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 400,
              "T24 duplicate Origin -> 400 (smuggling hardening)");
        _snprintf(rq, sizeof(rq),
                  "GET /api/v1/health HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n"
                  "Origin: http://evil.com\r\nConnection: close\r\n\r\n", g_port);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 403,
              "T24 layer independence: bad Origin rejected even without token (403 first)");
        _snprintf(rq, sizeof(rq),
                  "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nOrigin: http://evil.com\r\n"
                  "Connection: close\r\n\r\n", g_port);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 403,
              "T24 static face protected too (GET / bad Origin -> 403)");
        _snprintf(org, sizeof(org), "http://127.0.0.1:%d", g_port);
        _snprintf(rq, sizeof(rq),
                  "POST /api/v1/refresh HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                  "Origin: %s\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                  "Connection: close\r\n\r\n{}", g_port, TOKEN, org);
        CHECK(exchange(rq, (int)strlen(rq)) > 0 && resp_status() == 200 && has("\"ok\":true"),
              "T24 panel POST action with loopback Origin -> 200 ok");
    }

    /* ---------------- T25 b4 health manifestFingerprint (docs\23 4.2 source c) --------- */
    printf("--- T25 health manifestFingerprint (b4) ---\n");
    {
        char fp1[64], fp2[64], mf[512], orig[1024], mutated[1024];
        _snprintf(mf, sizeof(mf), "%s\\plugins\\p-ok\\plugin.json", g_troot);
        {
            FILE* f = fopen(mf, "rb");
            size_t rd = 0;
            if (f) { rd = fread(orig, 1, sizeof(orig) - 1, f); fclose(f); }
            orig[rd] = '\0';
            CHECK(rd > 0, "T25 read p-ok manifest for mutation test");
        }
        api_get("/api/v1/health", TOKEN);
        CHECK(resp_status() == 200 && grab_str("manifestFingerprint", fp1, sizeof(fp1))
              && strlen(fp1) == 16,
              "T25 health carries 16-hex manifestFingerprint");
        api_get("/api/v1/health", TOKEN);
        grab_str("manifestFingerprint", fp2, sizeof(fp2));
        CHECK(strcmp(fp1, fp2) == 0, "T25 fingerprint stable across polls (bridge 5s contract)");
        _snprintf(mutated, sizeof(mutated), "%s ", orig);
        write_file(mf, mutated);
        POST_OK("/api/v1/refresh", "{}");
        api_get("/api/v1/health", TOKEN);
        grab_str("manifestFingerprint", fp2, sizeof(fp2));
        CHECK(strcmp(fp1, fp2) != 0, "T25 manifest byte bump -> generation changes");
        write_file(mf, orig);
        POST_OK("/api/v1/refresh", "{}");
        api_get("/api/v1/health", TOKEN);
        grab_str("manifestFingerprint", fp2, sizeof(fp2));
        CHECK(strcmp(fp1, fp2) == 0, "T25 restore -> fingerprint returns (pure function of manifests)");
    }

    /* ---------------- T17 terminate phase 1: stop_waiters (E9/C5 stall fix) ---------------- */
    printf("--- T17 stop_waiters wakes in-flight POST waiter ---\n");
    {
        HANDLE th;
        DWORD wait_rc;
        g_pump_on = 0;                          /* freeze executor: pump is gone */
        web_api_set_executor_ready(1);          /* allow a submit so a waiter exists */
        memset(&g_t17, 0, sizeof(g_t17));
        th = CreateThread(NULL, 0, t17_client, NULL, 0, NULL);
        CHECK(th != NULL, "T17 client thread started");
        Sleep(500);                             /* let it block on the slot event */
        web_api_set_executor_ready(0);          /* terminate step 1 */
        web_api_stop_waiters();                 /* terminate step 1b: WAKE everyone */
        wait_rc = WaitForSingleObject(th, 5000);
        CloseHandle(th);
        CHECK(wait_rc == WAIT_OBJECT_0, "T17 client thread finished within 5s");
        CHECK(g_t17.done == 1, "T17 waiter completed");
        CHECK(g_t17.elapsed < 2500, "T17 woke fast (<2.5s; burns full 8s timeout without the fix)");
        CHECK(g_t17.status == 503, "T17 answered 503");
        CHECK(strstr(g_t17.body, "hub stopping") != NULL, "T17 body says 'hub stopping'");
        /* stopping window semantics: new POST rejected fast, GETs still served */
        POST_OK("/api/v1/mount", "{\"id\":\"p-ok\"}");
        CHECK(resp_status() == 503 && has("hub not ready"), "T17 new POST during stopping -> 503 not-ready");
        api_get("/api/v1/health", TOKEN);
        CHECK(resp_status() == 200 && has("\"ok\":true"), "T17 GET still served during stopping window");
        CHECK(web_api_queue_used() == 0, "T17 queue drained (waiter released its slot)");
        /* re-init resets the stopping flag (Aux Apps stop->start in one process) */
        web_api_shutdown();
        CHECK(web_api_init() == 1, "T17 re-init after shutdown");
        web_api_set_executor_ready(1);
        g_pump_on = 1;
        POST_OK("/api/v1/refresh", "{}");
        CHECK(resp_status() == 200 && has("\"ok\":true"), "T17 queue works again after re-init (refresh ok)");
    }

    /* ---------------- T-async (M4b): 202 + job table ---------------- */
    printf("--- T-async: async command -> 202 + jobs (M4b) ---\n");
    {
        char jid[32];
        char jpath[128];
        int waited = 0;

        api_get("/api/v1/health", TOKEN);
        CHECK(resp_status() == 200 && has("\"hubVersion\":\"1.0.0\""),
              "T-async health carries hubVersion");
        CHECK(has("\"jobs\":{\"running\":"), "T-async health carries job counters");

        POST_OK("/api/v1/call", "{\"plugin\":\"p-ok\",\"command\":\"p-ok.batch\"}");
        CHECK(resp_status() == 202, "T-async /call -> 202");
        CHECK(has("\"meta\":{\"async\":true"), "T-async meta.async=true");
        CHECK(has("\"/api/v1/jobs/"), "T-async poll endpoint advertised");
        CHECK(grab_str("jobId", jid, sizeof(jid)), "T-async jobId present");

        api_get("/api/v1/jobs?all=1", TOKEN);
        CHECK(resp_status() == 200 && has(jid), "T-async list contains the job");

        _snprintf(jpath, sizeof(jpath), "/api/v1/jobs/%s", jid);
        for (i = 0; i < 100 && !waited; i++) {
            api_get(jpath, TOKEN);
            if (has("\"state\":\"completed\"") || has("\"state\":\"failed\"")) waited = 1;
            else Sleep(50);
        }
        CHECK(waited, "T-async job reaches a terminal state (pump executed it)");
        CHECK(resp_status() == 200 && has("\"state\":\"completed\""),
              "T-async echo stub -> completed");
        CHECK(has("\"result\":"), "T-async job detail carries result");

        api_get("/api/v1/jobs/j-999999", TOKEN);
        CHECK(resp_status() == 404 && has("job not found"), "T-async unknown job -> 404");
    }

    /* ---------------- shutdown ---------------- */
    InterlockedExchange(&g_pump_run, 0);
    WaitForSingleObject(pump, 5000);
    CloseHandle(pump);
    httpd_stop();
    web_api_shutdown();
    if (g_hub_log_tee) { fclose(g_hub_log_tee); g_hub_log_tee = NULL; }
    {
        char st[MAX_PATH + 48];
        _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
        remove(st);
        remove(g_tee_path);
    }
    WSACleanup();
    printf("\n=== RESULT: %d pass, %d fail -> %s ===\n",
           g_pass, g_fail, g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
