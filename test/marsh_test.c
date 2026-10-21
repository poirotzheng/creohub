/* =====================================================================
 * creo-hub M2 OFFLINE test: hub_marsh.c (E6) + hub_token.c (G4) +
 * web_api integration through the marshaling window (gates B-1/B-2).
 *
 * A "main thread sim" owns the message-only window and runs a real
 * MsgWaitForMultipleObjects loop with a 500ms timeout - structurally the
 * same as Creo's main loop + the ProUITimer safety net:
 *   - message arrives   -> WndProc pumps (PostMessage path, E6 primary)
 *   - 500ms timeout     -> poll pump      (ProUITimer fallback path)
 * The test thread plays the httpd worker / user.
 * Exit code = number of failures (0 = ALL PASS).
 * ===================================================================== */
#include "web_api.h"
#include "hub_marsh.h"
#include "hub_token.h"
#include "hub_log.h"
#include "registry.h"
#include "reconcile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int g_fail = 0;
static int g_pass = 0;

extern int g_hub_log_quiet;

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

#define BASE_PORT 18790
static const char* TOKEN = "***";
static char g_troot[MAX_PATH];
static char g_resp[64 * 1024];
static int  g_port = 0;

/* ---- main-thread simulation state ---- */
static volatile LONG g_sim_run   = 1;
static volatile LONG g_poll_on   = 0;   /* ProUITimer fallback simulation */
static volatile LONG g_work      = 0;   /* phase-A work items */
static volatile LONG g_saw_main  = -1;  /* is_main_thread seen inside pump */
static int (*g_active_pump)(void) = NULL;

static int pump_dispatch(void) { return g_active_pump ? g_active_pump() : 0; }

static int pump_counter(void)
{
    if (InterlockedCompareExchange(&g_saw_main, hub_marsh_is_main_thread(), -1) == -1) {
        /* recorded on first pump call */
    }
    if (g_work > 0) { InterlockedDecrement(&g_work); return 1; }
    return 0;
}

static DWORD WINAPI main_sim_thread(LPVOID a)
{
    (void)a;
    if (!hub_marsh_init(pump_dispatch)) {
        printf("[FAIL] sim: hub_marsh_init\n");
        InterlockedIncrement(&g_fail);
        return 1;
    }
    while (g_sim_run) {
        MSG m;
        DWORD wr = MsgWaitForMultipleObjects(0, NULL, FALSE, 500, QS_ALLINPUT);
        if (wr == WAIT_OBJECT_0) {
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
        } else if (wr == WAIT_TIMEOUT) {
            if (g_poll_on) { while (pump_dispatch()) { } }
        }
    }
    hub_marsh_shutdown();
    return 0;
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

static void setup_root(void)
{
    char sub[MAX_PATH], st[MAX_PATH + 48];
    DWORD n = GetTempPathA(MAX_PATH, sub);
    sub[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_marsh", sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\m-ok\\bin", g_troot);            mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\m-ok\\text\\text\\usascii", g_troot); mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\m-ok\\logs", g_troot);           mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\logs", g_troot);                          mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\m-ok\\bin\\m-ok.dll", g_troot);
    write_file(sub, "MZ-fake");
    _snprintf(sub, sizeof(sub), "%s\\plugins\\m-ok\\text\\text\\usascii\\m-ok_messages.txt", g_troot);
    write_file(sub, "#MSG dummy\n");
    _snprintf(sub, sizeof(sub), "%s\\plugins\\m-ok\\plugin.json", g_troot);
    write_file(sub, "{\"manifestVersion\":1,\"id\":\"m-ok\",\"adapter\":\"ptk-sync\","
                    "\"entry\":\"bin/m-ok.dll\",\"textDir\":\"text\"}");
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
    _snprintf(st, sizeof(st), "%s\\hub_token.txt", g_troot);
    remove(st);
    strcpy(g_root, g_troot);
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

static int has(const char* needle) { return strstr(g_resp, needle) != NULL; }

static int api_req(const char* method, const char* path, const char* body)
{
    static char req[4096];
    int n;
    if (body)
        n = _snprintf(req, sizeof(req),
                      "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                      "Content-Type: application/json\r\nContent-Length: %d\r\n"
                      "Connection: close\r\n\r\n%s",
                      method, path, g_port, TOKEN, (int)strlen(body), body);
    else
        n = _snprintf(req, sizeof(req),
                      "%s %s HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nX-Hub-Token: %s\r\n"
                      "Connection: close\r\n\r\n",
                      method, path, g_port, TOKEN);
    return exchange(req, n);
}

static long get_seq(void)
{
    const char* p = strstr(g_resp, "\"seq\":");
    return p ? atol(p + 6) : -1;
}

/* ===================================================================== */

int main(void)
{
    WSADATA wsa;
    HANDLE sim = NULL;
    int port = 0;
    long s1, s2;
    DWORD t0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup fail\n"); return 2; }
    printf("=== creo-hub M2 marshaling+token OFFLINE test (E6/G4, B-1/B-2) ===\n\n");
    setup_root();
    g_hub_log_quiet = 1;

    /* ---------------- A: pre-init guards ---------------- */
    printf("--- A pre-init ---\n");
    CHECK(hub_marsh_post() == 0, "E6 post before init -> 0 (no window yet)");
    CHECK(hub_marsh_is_main_thread() == 0, "E6 is_main_thread before init -> 0");

    /* ---------------- B: pure marshaling (counter pump) ---------------- */
    printf("--- B marshaling window (PostMessage path) ---\n");
    g_active_pump = pump_counter;
    InterlockedExchange(&g_poll_on, 0);
    sim = CreateThread(NULL, 0, main_sim_thread, NULL, 0, NULL);
    if (!sim) { printf("cannot create sim thread\n"); return 2; }
    Sleep(200);   /* let the window come up */
    CHECK(hub_marsh_is_main_thread() == 0, "E6 test thread is NOT the owner");

    InterlockedExchange(&g_work, 1);
    t0 = GetTickCount();
    CHECK(hub_marsh_post() == 1, "E6 post from foreign thread accepted");
    while (g_work > 0 && GetTickCount() - t0 < 1000) Sleep(10);
    CHECK(g_work == 0, "E6 PostMessage woke owner thread and pumped (poll OFF)");
    CHECK(GetTickCount() - t0 < 400, "E6 wake latency < 400ms (message path, not the 500ms poll)");
    CHECK(g_saw_main == 1, "E6 pump callback runs ON the owner (main) thread");

    InterlockedExchange(&g_work, 500);
    t0 = GetTickCount();
    {
        int k;
        for (k = 0; k < 500; k++) hub_marsh_post();   /* burst: coalescing-safe */
    }
    while (g_work > 0 && GetTickCount() - t0 < 3000) Sleep(10);
    CHECK(g_work == 0, "E6 burst of 500 posts fully drained (no lost work)");

    /* ---------------- C: web_api through the window (E6 primary) ---------------- */
    printf("--- C web_api integration via E6 window ---\n");
    CHECK(web_api_init() == 1, "web_api_init");
    registry_scan(g_troot);
    CHECK(g_n_plugins == 1, "fixture scanned (m-ok)");
    web_api_set_token(TOKEN);
    web_api_set_wakeup(hub_marsh_post);
    httpd_set_api_handler(web_api_route);
    CHECK(httpd_start(BASE_PORT, &port) == 1, "httpd start");
    g_port = port;
    web_api_set_executor_ready(1);
    g_active_pump = web_api_run_pending;   /* switch pump to the real executor */
    InterlockedExchange(&g_poll_on, 0);    /* PROVE the PostMessage path alone */

    t0 = GetTickCount();
    CHECK(api_req("POST", "/api/v1/mount", "{\"id\":\"m-ok\"}") > 0 &&
          resp_status() == 200 && has("\"ok\":true") && has("\"state\":\"ACTIVE\""),
          "E6+G1 HTTP mount executed ON the main-sim thread via PostMessage wake");
    s1 = get_seq();
    CHECK(GetTickCount() - t0 < 1500, "E6 mount latency < 1.5s with polling DISABLED");

    CHECK(api_req("GET", "/api/v1/health", NULL) > 0 && resp_status() == 200 &&
          has("\"ok\":true") && has("\"executorReady\":true"),
          "health still fine through the same stack");

    /* ---------------- D: fallback path (poll only, like ProUITimer) ---------------- */
    printf("--- D fallback: 500ms poll only (X3 degradation path) ---\n");
    web_api_set_wakeup(NULL);              /* no PostMessage anymore */
    InterlockedExchange(&g_poll_on, 1);    /* owner polls every 500ms */
    t0 = GetTickCount();
    CHECK(api_req("POST", "/api/v1/unmount", "{\"id\":\"m-ok\"}") > 0 &&
          resp_status() == 200 && has("\"ok\":true") && has("\"state\":\"REGISTERED\""),
          "G1 op completes with polling ONLY (wakeup lost => timer safety net)");
    s2 = get_seq();
    CHECK(GetTickCount() - t0 < 2000, "fallback latency < 2s (one or two 500ms ticks)");
    CHECK(s1 >= 0 && s2 > s1, "G1 seq monotonic across both paths");
    web_api_set_wakeup(hub_marsh_post);    /* restore primary path */

    /* ---------------- E: G4 token ---------------- */
    printf("--- E token (G4) ---\n");
    {
        char t1[64], t2[64], path[MAX_PATH + 48];
        char* fx;
        int hexok = 1, i;
        CHECK(hub_token_generate() == 1, "G4 token generated");
        strncpy(t1, hub_token_value(), sizeof(t1) - 1); t1[63] = 0;
        CHECK(strlen(t1) == 32, "G4 token is 32 hex chars (128-bit)");
        for (i = 0; i < 32; i++) if (!isxdigit((unsigned char)t1[i])) hexok = 0;
        CHECK(hexok, "G4 token all-hex");
        hub_token_generate();
        strncpy(t2, hub_token_value(), sizeof(t2) - 1); t2[63] = 0;
        CHECK(strcmp(t1, t2) != 0, "G4 rotation: second generate differs");
        CHECK(hub_token_write_file(g_troot) == 1, "G4 token file written");
        _snprintf(path, sizeof(path), "%s\\hub_token.txt", g_troot);
        fx = read_file_alloc(path);
        CHECK(fx && strncmp(fx, t2, 32) == 0, "G4 file contains current token");
        free(fx);
        hub_token_generate();               /* rotate again */
        CHECK(hub_token_write_file(g_troot) == 1, "G4 re-write (rotation) ok");
        fx = read_file_alloc(path);
        CHECK(fx && strncmp(fx, hub_token_value(), 32) == 0 && strncmp(fx, t2, 32) != 0,
              "G4 file overwritten with NEW token (old invalidated)");
        free(fx);
        CHECK(hub_token_delete_file(g_troot) == 1, "G4 delete ok");
        CHECK(read_file_alloc(path) == NULL, "G4 file gone after delete");
        CHECK(hub_token_delete_file(g_troot) == 1, "G4 delete of missing file = success (idempotent)");
    }

    /* ---------------- shutdown ---------------- */
    printf("--- F shutdown ---\n");
    web_api_set_executor_ready(0);
    web_api_stop_waiters();     /* two-phase quiesce: same order as hub_main */
    httpd_stop();
    CHECK(web_api_queue_used() == 0, "queue empty at shutdown");
    web_api_shutdown();
    InterlockedExchange(&g_sim_run, 0);
    /* wake the sim out of MsgWait (it polls g_sim_run every <=500ms anyway) */
    hub_marsh_post();
    CHECK(WaitForSingleObject(sim, 5000) == WAIT_OBJECT_0, "main-sim thread exits cleanly");
    CloseHandle(sim);
    {
        char st[MAX_PATH + 48];
        _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
        remove(st);
    }
    WSACleanup();
    printf("\n=== RESULT: %d pass, %d fail -> %s ===\n",
           g_pass, g_fail, g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
