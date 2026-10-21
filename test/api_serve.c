/* =====================================================================
 * creo-hub M2 test servant: api_serve.exe <port> <seconds> <token> <panelSrc>
 * Brings up the FULL production stack offline (registry + reconcile +
 * web_api + httpd + pump thread as executor) on a hermetic temp root,
 * copies the real panel file to <root>\web\index.html, seeds a log file,
 * serves for <seconds>, then exits 0 (auto-exit: no taskkill needed).
 * Used by run_panel_e2e.cmd for headless-Chrome + node contract checks.
 * ===================================================================== */
#include "web_api.h"
#include "hub_log.h"
#include "registry.h"
#include "reconcile.h"
#include "nonce.h"        /* b4: seed a live ticket for the confirm-dialog dump */
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int g_hub_log_quiet;
extern int g_stub_dispatch_on;   /* M3c c1: test_stubs callfw behaviour dispatch */

static char g_troot[MAX_PATH];
static volatile LONG g_run = 1;

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
    char p[MAX_PATH + 160], json[MAX_PATH + 64];
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\bin", g_troot, dir);            mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\text\\text\\usascii", g_troot, dir); mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\bin\\%s", g_troot, dir, entry_name);
    write_file(p, "MZ-fake");
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\text\\text\\usascii\\%s_messages.txt", g_troot, dir, dir);
    write_file(p, "#MSG dummy\n");
    _snprintf(json, sizeof(json), "%s\\plugins\\%s\\plugin.json", g_troot, dir);
    write_file(json, manifest);
}

/* b4: version dir under a pointer package (b3 P1 layout, immutable dirs) */
static void mkver(const char* dir, const char* ver, const char* cmdid)
{
    char p[MAX_PATH + 192], json[1024];
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\versions\\%s\\bin", g_troot, dir, ver); mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\versions\\%s\\text\\text\\usascii", g_troot, dir, ver); mkdir_p(p);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\versions\\%s\\bin\\%s.dll", g_troot, dir, ver, dir);
    write_file(p, "MZ-fake");
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\versions\\%s\\text\\text\\usascii\\%s_messages.txt",
              g_troot, dir, ver, dir);
    write_file(p, "#MSG dummy\n");
    _snprintf(json, sizeof(json),
              "{\"manifestVersion\":2,\"id\":\"%s\",\"name\":\"Demo Versioned\",\"version\":\"%s\","
              "\"adapter\":\"ptk-sync\",\"entry\":\"bin/%s.dll\",\"textDir\":\"text\","
              "\"commands\":[{\"id\":\"%s\",\"title\":\"Ver %s\",\"desc\":\"v%s command\","
              "\"descForLLM\":\"versioned fixture command %s\",\"risk\":\"safe\","
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}",
              dir, ver, dir, cmdid, ver, ver, ver);
    _snprintf(p, sizeof(p), "%s\\plugins\\%s\\versions\\%s\\plugin.json", g_troot, dir, ver);
    write_file(p, json);
}

static DWORD WINAPI pump_thread(LPVOID a)
{
    (void)a;
    while (g_run) {
        while (web_api_run_pending()) { }
        Sleep(5);
    }
    return 0;
}

int main(int argc, char** argv)
{
    char sub[MAX_PATH], web[MAX_PATH + 32], dst[MAX_PATH + 64], logf[MAX_PATH + 48];
    int port_pref, seconds;
    int port = 0;
    HANDLE pump;
    DWORD n;

    if (argc < 5) {
        printf("usage: api_serve.exe <port> <seconds> <token> <panelSrc>"
               " [realPluginsDir] [realLogFile] [callfw]\n");
        return 2;
    }
    port_pref = atoi(argv[1]);
    seconds = atoi(argv[2]);
    g_hub_log_quiet = 1;
    /* M3c c1: "callfw" (argv[7]) adds the p-call command-behaviour fixture
     * package and switches the adapter stubs to command-id dispatch. Default
     * (panel_e2e) firmware stays byte-identical to the b4 shape. */
    if (argc > 7 && strcmp(argv[7], "callfw") == 0) {
        g_stub_dispatch_on = 1;
        printf("CALLFW on\n");
    }

    n = GetTempPathA(MAX_PATH, sub);
    sub[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_panel", sub);
    /* M3c c1: hermetic per-run - wipe stale fixture plugins from the shared
     * temp root BEFORE seeding (battery callfw mode and panel_e2e default
     * mode alternate on this root; leftovers shift package counts - the
     * p-call residue that broke contract "count=4" was exactly this). */
    _snprintf(sub, sizeof(sub), "rd /s /q \"%s\\plugins\" 2>nul", g_troot);
    system(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins", g_troot); mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\logs", g_troot);    mkdir_p(sub);
    _snprintf(web, sizeof(web), "%s\\web", g_troot);     mkdir_p(web);

    /* panel under test: the REAL repo file, copied fresh */
    _snprintf(dst, sizeof(dst), "%s\\index.html", web);
    if (!CopyFileA(argv[4], dst, FALSE)) {
        printf("cannot copy panel %s (err=%lu)\n", argv[4], GetLastError());
        return 2;
    }

    /* optional REAL-DATA mode (field-issue repro): argv[5]=real plugins dir
     * (copied whole, replacing fixtures), argv[6]=real hub log file. Lets the
     * headless-browser E2E run against the exact shapes production returns. */
    if (argc > 5 && argv[5][0] != '-' && argv[5][0]) {
        char cmd[2 * MAX_PATH + 96];
        _snprintf(cmd, sizeof(cmd),
                  "xcopy \"%s\" \"%s\\plugins\" /E /I /Y >nul 2>&1", argv[5], g_troot);
        if (system(cmd) != 0) { printf("REAL-PLUGINS copy failed: %s\n", argv[5]); return 2; }
        printf("REAL-PLUGINS %s\n", argv[5]);
    } else {
    mkpkg("p-ok", "p-ok.dll",
          "{\"manifestVersion\":2,\"id\":\"p-ok\",\"name\":\"Demo OK\",\"version\":\"1.2.0\","
          "\"adapter\":\"ptk-sync\",\"entry\":\"bin/p-ok.dll\",\"textDir\":\"text\","
          "\"commands\":["
          "{\"id\":\"p-ok.hello\",\"title\":\"Hello\",\"desc\":\"say hello\",\"descForLLM\":\"Say hello; takes no arguments\",\"risk\":\"safe\","
          "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}},"
          "{\"id\":\"p-ok.purge\",\"title\":\"Purge cache\",\"desc\":\"delete temp cache\",\"descForLLM\":\"Purge the plugin temp cache; irreversible\",\"risk\":\"destructive\",\"timeoutSec\":30,\"reversible\":false,\"idempotent\":true,"
          "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"dry\":{\"type\":\"boolean\",\"description\":\"dry run\"}},\"required\":[\"dry\"]}}],"
          "\"requires\":[\"core.base\"],\"provides\":[\"ok.svc\"]}");
    mkpkg("p-fail", "demo_fail.dll",
          "{\"manifestVersion\":1,\"id\":\"p-fail\",\"adapter\":\"ptk-sync\","
          "\"entry\":\"bin/demo_fail.dll\",\"textDir\":\"text\"}");
    mkpkg("p-bad", "p-bad.dll", "{\"manifestVersion\":1}");
    /* b4: pointer package so the panel version column + switch/rollback
     * buttons render in the dump, and the contract can drive /update and
     * /rollback over the wire. */
    {
        char p[MAX_PATH + 160];
        _snprintf(p, sizeof(p), "%s\\plugins\\p-ver", g_troot); mkdir_p(p);
        _snprintf(p, sizeof(p), "%s\\plugins\\p-ver\\plugin.json", g_troot);
        write_file(p, "{\"manifestVersion\":2,\"id\":\"p-ver\",\"adapter\":\"ptk-sync\","
                      "\"currentVersion\":\"1.0.0\"}");
        mkver("p-ver", "1.0.0", "p-ver.one");
        mkver("p-ver", "1.1.0", "p-ver.eleven");
    }
    /* M3c c1 (callfw only): command-behaviour fixture for bridge battery
     * I/J - echo/fail/slow dispatch by command-id suffix in test_stubs.c,
     * nuke = destructive for the nonce/confirm chain. Separated from p-ok
     * so panel_e2e asserts (command counts, purge ticket) never see it. */
    if (g_stub_dispatch_on) {
        mkpkg("p-call", "p-call.dll",
              "{\"manifestVersion\":2,\"id\":\"p-call\",\"name\":\"Call Firmware\",\"version\":\"1.0.0\","
              "\"adapter\":\"ptk-sync\",\"entry\":\"bin/p-call.dll\",\"textDir\":\"text\","
              "\"commands\":["
              "{\"id\":\"p-call.echo\",\"title\":\"Echo\",\"desc\":\"echo args back\","
              "\"descForLLM\":\"Echo the arguments back verbatim; offline battery fixture\",\"risk\":\"safe\","
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\",\"description\":\"message to echo\"}},\"required\":[\"msg\"],\"additionalProperties\":false}},"
              "{\"id\":\"p-call.fail\",\"title\":\"Fail\",\"desc\":\"staged business failure\","
              "\"descForLLM\":\"Always returns a staged business failure; offline battery fixture\",\"risk\":\"safe\",\"idempotent\":true,"
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
              "{\"id\":\"p-call.slow\",\"title\":\"Slow\",\"desc\":\"staged slow command\","
              "\"descForLLM\":\"Sleeps ~2.5s before answering; 504/orphan battery fixture\",\"risk\":\"safe\",\"timeoutSec\":1,"
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}},"
              "{\"id\":\"p-call.nuke\",\"title\":\"Nuke\",\"desc\":\"staged destructive\","
              "\"descForLLM\":\"Destructive fixture requiring human confirmation; nonce battery fixture\",\"risk\":\"destructive\",\"timeoutSec\":30,\"reversible\":false,\"idempotent\":true,"
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"target\":{\"type\":\"string\",\"description\":\"target name\"}},\"required\":[\"target\"]}},"
              "{\"id\":\"p-call.ask\",\"title\":\"Ask\",\"desc\":\"staged confirm-risk command\","
              "\"descForLLM\":\"Confirm-risk fixture: human-gated, panel optional; elicitation battery L\",\"risk\":\"confirm\",\"idempotent\":true,"
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"note\":{\"type\":\"string\",\"description\":\"note to pass through\"}},\"required\":[\"note\"]}},"
              "{\"id\":\"p-call.job\",\"title\":\"Job\",\"desc\":\"async long task\","
              "\"descForLLM\":\"Async fixture (M4b): call answers 202 + jobId; poll GET /api/v1/jobs/<id> for the result\",\"risk\":\"safe\",\"async\":true,\"timeoutSec\":60,"
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}"
              "]}");
    }
    }

    /* seed log so the console pane has content (stub hub_log does not
     * write files; the /log endpoint reads this path directly) */
    _snprintf(logf, sizeof(logf), "%s\\logs\\hub_m1.log", g_troot);
    if (argc > 6 && argv[6][0] != '-' && argv[6][0]) {
        if (!CopyFileA(argv[6], logf, FALSE)) {
            printf("REAL-LOG copy failed (err=%lu)\n", GetLastError());
            return 2;
        }
        printf("REAL-LOG %s\n", argv[6]);
    } else {
        /* 500-line seed (R13): 495 fillers FIRST, then the 5 scenario lines.
         * Exercises the panel LOG_CAP(400) trim path end-to-end - the field
         * P5 bug (trim wiped the whole box) was invisible offline because the
         * old seed had only 5 lines. Dump asserts: MARKER-000 trimmed away,
         * MARKER-494 + [AUDIT] line visible. */
        static char big[96 * 1024];
        int pos = 0, k;
        for (k = 0; k < 495; k++)
            pos += _snprintf(big + pos, sizeof(big) - pos,
                             "[08:%02d:%02d] [seed] filler (LOGTRIM-MARKER-%03d)\n",
                             k / 60, k % 60, k);
        pos += _snprintf(big + pos, sizeof(big) - pos,
                "[09:00:01] [hub] === creo_hub M2 user_initialize: root=C:\\demo ===\n"
                "[09:00:01] [REG] scanned 3 package(s)\n"
                "[09:00:02] [AUDIT] op=mount plugin=p-ok args={\"id\":\"p-ok\"} result=ACTIVE err=NO_ERROR ms=18\n"
                "[09:00:03] [REG] manifest fingerprint CHANGED for p-ok (badge set, no auto-remount)\n"
                "[09:00:04] [HTTP] listening on 127.0.0.1:8765\n");
        big[pos] = '\0';
        write_file(logf, big);
    }
    _snprintf(sub, sizeof(sub), "%s\\hub_state.json", g_troot);
    remove(sub);

    strcpy(g_root, g_troot);

    if (!web_api_init()) { printf("web_api_init failed\n"); return 2; }
    registry_scan(g_troot);
    {   /* b4: seed ONE live ISSUED destructive ticket (p-ok.purge) so the
         * headless dump renders the confirm dialog and the contract can
         * approve it through /api/v1/pending + /api/v1/confirm. */
        int pi = registry_find_idx("p-ok");
        if (pi >= 0) {
            int ci;
            for (ci = 0; ci < g_plugins[pi].n_commands; ci++)
                if (strcmp(g_plugins[pi].commands[ci], "p-ok.purge") == 0) break;
            if (ci < g_plugins[pi].n_commands) {
                NonceTicket t;
                cJSON* args = cJSON_CreateObject();
                cJSON_AddBoolToObject(args, "dry", 1);
                if (nonce_issue(pi, ci, nonce_args_digest(args), "destructive", &t) == NR_OK)
                    printf("SEEDED pending txn=%s nonce_id=%.8s\n", t.txn, t.nonce);
                else
                    printf("SEED-FAILED nonce_issue\n");
                cJSON_Delete(args);
            }
        }
    }
    web_api_set_token(argv[3]);
    web_api_set_executor_ready(1);
    httpd_set_api_handler(web_api_route);
    if (!httpd_start(port_pref, &port)) { printf("httpd_start failed\n"); return 2; }
    pump = CreateThread(NULL, 0, pump_thread, NULL, 0, NULL);
    printf("SERVING %d root=%s panel=%s\n", port, g_troot, dst);
    fflush(stdout);

    Sleep((DWORD)seconds * 1000);

    InterlockedExchange(&g_run, 0);
    WaitForSingleObject(pump, 5000);
    CloseHandle(pump);
    web_api_set_executor_ready(0);
    web_api_stop_waiters();     /* two-phase quiesce: same order as hub_main */
    httpd_stop();
    web_api_shutdown();
    _snprintf(sub, sizeof(sub), "%s\\hub_state.json", g_troot);
    remove(sub);
    printf("SERVE-DONE\n");
    return 0;
}
