/* =====================================================================
 * creo-hub M2: web_api.c - HTTP API face.
 * Contract (docs\09 4): 200 + ok:false = business failure, 503 = busy /
 * hub not ready, 400 = JSON/args parse failure, 401 = token, 411/415 =
 * POST body discipline (L4), 404 = unknown endpoint (L8 fixed table).
 * G1 envelope: {op_type, payload_json, seq, timeout_ms, result_json, event}
 * G5 health: {ok, apiVersion:"v1", features:["panel","manifest"], ...}
 * R3 audit: one-line [AUDIT] records via hub_log for every POST outcome
 * and every security rejection (GETs are NOT audited - 2s poll storm).
 * ===================================================================== */
#include "web_api.h"
#include "hub_log.h"
#include "registry.h"
#include "reconcile.h"
#include "call_exec.h"
#include "nonce.h"
#include "opmode.h"
#include "version_mgr.h"
#include "jobs.h"        /* M4b: async job table (docs\39 3.2) */
#include "cJSON.h"
#ifdef CREO_HUB_DEV
#include "dev_e11.h"   /* M3a experiment round only; production never compiles it */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define TAG     "API"
#define AUD     "AUDIT"
#define HUB_VERSION_STR "1.0.0"   /* M4: added to health (docs\39 C11) */

#define Q_SLOTS           16
#define DEFAULT_TIMEOUT_MS 10000   /* docs\09: G1 default */
#define MAX_TIMEOUT_MS     30000
#ifdef CREO_HUB_DEV
/* M3a dev build only: E11-4 pushes a ~64KB in_json through the envelope and
 * the OUT_JSON echo comes back the same size. Production caps are untouched
 * (the #else branch is byte-identical to the M2 frozen values). */
#define RESULT_CAP         (320 * 1024)
#define PAYLOAD_CAP        (160 * 1024)
#else
/* M3b b1: /call productization raises the M2 control-verb caps - business
 * OUT_JSON/args do not fit 4KB/2KB. 64KB matches the httpd body cap and the
 * E11-4 verified ProArgument string limit. Static cost: 16 slots x 128KB = 2MB. */
#define RESULT_CAP         65536
#define PAYLOAD_CAP        65536
#endif
#define LOG_TAIL_CAP       (256 * 1024)

typedef struct {
    int  used;
    long seq;
    char op_type[32];
    char payload_json[PAYLOAD_CAP];
    int  timeout_ms;
    HANDLE event;                 /* manual-reset, created once at init */
    volatile LONG taken;          /* executor claimed it */
    volatile LONG executed;       /* result ready */
    volatile LONG abandoned;      /* worker timed out and left */
    int  status;                  /* HTTP status for result_json */
    char result_json[RESULT_CAP];
    char plugin_id[64];           /* audit fields filled by executor */
    char result_state[160];       /* b2: carries txn/nonce_id audit fragments */
    volatile LONG started;        /* b2 orphan protocol: executor began the run */
} WebOpSlot;

static WebOpSlot        g_q[Q_SLOTS];
static CRITICAL_SECTION g_cs;
static int              g_inited = 0;
static long             g_seq = 0;
static char             g_token[64] = "";
static volatile LONG    g_executor_ready = 0;
static volatile LONG    g_stopping = 0;      /* terminate phase 1 (R7b/E9):
                                              * set by web_api_stop_waiters;
                                              * reset by web_api_init */
static volatile LONG    g_orphaned = 0;      /* b2 orphan protocol (docs\22 §6.4):
                                              * 1 = a /call envelope timed out while its
                                              * command still owns the main thread; cleared
                                              * by the pump's late completion (call.late) */
static void           (*g_wakeup)(void) = NULL;
static DWORD            g_start_tick = 0;

/* ------------------------------------------------------------ lifecycle */

int web_api_init(void)
{
    int i;
    if (g_inited) return 1;
    InitializeCriticalSection(&g_cs);
    memset(g_q, 0, sizeof(g_q));
    for (i = 0; i < Q_SLOTS; i++) {
        g_q[i].event = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!g_q[i].event) {
            hub_log(TAG, "init failed: CreateEvent slot %d (%lu)", i, GetLastError());
            web_api_shutdown();
            return 0;
        }
    }
    g_start_tick = GetTickCount();
    nonce_init();                      /* b2 nonce gate table */
    jobs_init();                       /* M4b async job table */
    InterlockedExchange(&g_stopping, 0);
    InterlockedExchange(&g_orphaned, 0);
    g_inited = 1;
    hub_log(TAG, "ready (queue slots=%d, default timeout=%dms)", Q_SLOTS, DEFAULT_TIMEOUT_MS);
    return 1;
}

void web_api_shutdown(void)
{
    int i;
    jobs_shutdown();                   /* M4b: free job slots/mappings */
    for (i = 0; i < Q_SLOTS; i++) {
        if (g_q[i].event) { CloseHandle(g_q[i].event); g_q[i].event = NULL; }
    }
    if (g_inited) {
        DeleteCriticalSection(&g_cs);
        g_inited = 0;
        hub_log(TAG, "shutdown");
    }
}

/* Terminate phase 1 (two-phase quiesce, E9/C5 stall fix).
 * Problem: user_terminate stops the pump BEFORE httpd_stop() joins workers;
 * an in-flight POST waiter would then block for its full timeout (10..30s),
 * stalling the join and making the Aux Apps "Stop" look hung.
 * Fix: flag g_stopping + SetEvent every slot. Waiters wake immediately and
 * answer 503 "hub stopping"; new submits are rejected (executor_ready=0 and
 * g_stopping). Handles stay VALID here - CloseHandle/DeleteCriticalSection
 * remain in web_api_shutdown(), which hub_main only calls AFTER httpd_stop()
 * has joined every worker (no waiter can touch a closed handle). */
void web_api_stop_waiters(void)
{
    int i;
    if (!g_inited) return;
    InterlockedExchange(&g_stopping, 1);
    EnterCriticalSection(&g_cs);
    for (i = 0; i < Q_SLOTS; i++) {
        if (g_q[i].used && !g_q[i].executed)
            g_q[i].abandoned = 1;   /* executor drops it if a pump ever returns */
        if (g_q[i].event) SetEvent(g_q[i].event);
    }
    LeaveCriticalSection(&g_cs);
    hub_log(TAG, "stop_waiters: all slots signaled (terminate phase 1)");
}

void web_api_set_token(const char* hex_token)
{
    EnterCriticalSection(&g_cs);
    strncpy(g_token, hex_token ? hex_token : "", sizeof(g_token) - 1);
    g_token[sizeof(g_token) - 1] = '\0';
    LeaveCriticalSection(&g_cs);
    hub_log(TAG, "token %s (value never logged)", g_token[0] ? "set" : "cleared");
}

void web_api_set_executor_ready(int ready)
{
    InterlockedExchange(&g_executor_ready, ready ? 1 : 0);
    hub_log(TAG, "executor ready=%d", ready ? 1 : 0);
}

void web_api_set_wakeup(void (*fn)(void))
{
    g_wakeup = fn;   /* set once before httpd_start; read-only afterwards */
    hub_log(TAG, "wakeup hook %s", fn ? "registered (E6 PostMessage)" : "cleared (poll mode)");
}

int web_api_queue_used(void)
{
    int i, n = 0;
    if (!g_inited) return 0;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < Q_SLOTS; i++) if (g_q[i].used) n++;
    LeaveCriticalSection(&g_cs);
    return n;
}

/* ------------------------------------------------------------ audit R3 */

/* One line per audited event (kept single-line: CR/LF in args scrubbed):
 *   [AUDIT] op=<verb> plugin=<id|-> args=<json|-> result=<text> ms=<n>
 * Security rejections use op=<METHOD path> result=REJECT(<why>). */
static void audit(const char* op, const char* plugin, const char* args,
                  const char* result, DWORD ms)
{
    char a[520];
    char* p;
    strncpy(a, args ? args : "", sizeof(a) - 1);
    a[sizeof(a) - 1] = '\0';
    for (p = a; *p; p++)
        if (*p == '\n' || *p == '\r') *p = ' ';
    /* ISSUE-L1 (L-M3b-1 live finding): a client-presented confirm_nonce must
     * never land in the log in full - same rule as hub-issued nonces. Mask
     * everything past the first 8 hex (= nonce_id) in every occurrence.
     * Single choke point: every args= fragment passes through here. */
    for (p = a; (p = strstr(p, "\"confirm_nonce\":\"")) != NULL; ) {
        char* v = p + 17;
        int i = 0;
        while (v[i] && v[i] != '"') {
            if (i >= 8) v[i] = '*';
            i++;
        }
        p = v + i;
    }
    hub_log(AUD, "op=%s plugin=%s args=%s result=%s ms=%lu",
            op, (plugin && *plugin) ? plugin : "-",
            a[0] ? a : "-",
            (result && *result) ? result : "-",
            (unsigned long)ms);
}

/* ------------------------------------------------------------ JSON helpers */

static void respond_cjson(HttpdConn* c, int status, cJSON* obj)
{
    char* txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!txt) {
        httpd_respond_json(c, 500, "{\"ok\":false,\"error\":\"serialize failed\"}");
        return;
    }
    httpd_respond_json(c, status, txt);
    cJSON_free(txt);
}

static void respond_err(HttpdConn* c, int status, const char* msg)
{
    char buf[256];
    _snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}", msg);
    buf[sizeof(buf) - 1] = '\0';
    httpd_respond_json(c, status, buf);
}

static void add_str_array(cJSON* parent, const char* key,
                          const char (*items)[64], int n)
{
    cJSON* arr = cJSON_AddArrayToObject(parent, key);
    int i;
    for (i = 0; i < n; i++) cJSON_AddItemToArray(arr, cJSON_CreateString(items[i]));
}

/* ------------------------------------------------------------ GET routes */

static void get_health(HttpdConn* conn)
{
    cJSON* r = cJSON_CreateObject();
    cJSON* f = cJSON_AddArrayToObject(r, "features");
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddStringToObject(r, "apiVersion", "v1");
    cJSON_AddItemToArray(f, cJSON_CreateString("panel"));     /* G5 */
    cJSON_AddItemToArray(f, cJSON_CreateString("manifest"));  /* G5 */
    cJSON_AddItemToArray(f, cJSON_CreateString("call"));      /* M3b b1 */
    cJSON_AddItemToArray(f, cJSON_CreateString("versioning")); /* M3b b3 P1 */
    cJSON_AddItemToArray(f, cJSON_CreateString("jobs"));      /* M4b async */
#ifdef CREO_HUB_DEV
    cJSON_AddItemToArray(f, cJSON_CreateString("dev"));       /* M3b R5 (ISSUE-C1 fix) */
#endif
    cJSON_AddNumberToObject(r, "port", httpd_port());
    cJSON_AddNumberToObject(r, "uptimeSec", (double)((GetTickCount() - g_start_tick) / 1000));
    {
        int orphan = InterlockedCompareExchange(&g_orphaned, 1, 1);
        cJSON_AddBoolToObject(r, "executorReady", (g_executor_ready && !orphan) ? 1 : 0);
        cJSON_AddBoolToObject(r, "orphaned", orphan);   /* b2 (§6.4) */
    }
    cJSON_AddNumberToObject(r, "queueUsed", web_api_queue_used());
    cJSON_AddStringToObject(r, "hubVersion", HUB_VERSION_STR);   /* M4 C11 */
    {   /* M4b: async job counters (queryable even while executorReady=false) */
        int jrun = 0, jq = 0;
        jobs_count(&jrun, &jq);
        {
            cJSON* jj = cJSON_AddObjectToObject(r, "jobs");
            cJSON_AddNumberToObject(jj, "running", jrun);
            cJSON_AddNumberToObject(jj, "queued", jq);
        }
    }
    {   /* M3e Q1: operator mode menu status (panel banner + bridge visibility) */
        int om = 0;
        unsigned long long orem = 0;
        opmode_get(&om, &orem);
        cJSON_AddNumberToObject(r, "opmode", om);
        cJSON_AddNumberToObject(r, "opmodeRemainMs", (double)orem);
    }
    {   /* b4 (docs\23 4.2 trigger source c): global manifest generation =
         * FNV-1a combination of every package fingerprint in registry order,
         * 16-hex. Bridge polls health (5s, <1KB) and re-pulls the manifest
         * whenever this value changes - notification-free reconciliation. */
        unsigned long long h = 14695981039346656037ULL;
        char fpbuf[17];
        int fi, fb;
        for (fi = 0; fi < g_n_plugins; fi++) {
            if (!g_plugins[fi].id[0]) continue;
            for (fb = 0; fb < 8; fb++) {
                h ^= (unsigned char)((g_plugins[fi].fingerprint >> (fb * 8)) & 0xFF);
                h *= 1099511628211ULL;
            }
        }
        _snprintf(fpbuf, sizeof(fpbuf), "%016llx", h);
        cJSON_AddStringToObject(r, "manifestFingerprint", fpbuf);
    }
    respond_cjson(conn, 200, r);
}

/* M4b: async job read face (worker thread; never touches the main thread) */
static void get_jobs(HttpdConn* conn, const HttpdRequest* req)
{
    cJSON* r = cJSON_CreateObject();
    cJSON* arr;
    int all = (req && req->query[0] && strstr(req->query, "all=1")) ? 1 : 0;
    cJSON_AddBoolToObject(r, "ok", 1);
    arr = jobs_json_list(all);
    if (!arr) arr = cJSON_CreateArray();
    cJSON_AddNumberToObject(r, "count", cJSON_GetArraySize(arr));
    cJSON_AddItemToObject(r, "jobs", arr);
    respond_cjson(conn, 200, r);
}

static void get_job_one(HttpdConn* conn, const char* id)
{
    int idx = jobs_find(id);
    if (idx < 0) {
        cJSON* r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 0);
        cJSON_AddStringToObject(r, "error", "job not found");
        cJSON_AddStringToObject(r, "hint",
            "jobs are kept 600s after completion; GET /api/v1/jobs?all=1 lists recent ones");
        respond_cjson(conn, 404, r);
        return;
    }
    {
        cJSON* r = cJSON_CreateObject();
        cJSON* o = jobs_json_one(idx, 1);
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddItemToObject(r, "job", o ? o : cJSON_CreateObject());
        respond_cjson(conn, 200, r);
    }
}

static void get_plugins(HttpdConn* conn)
{
    cJSON* r = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(r, "plugins");
    int i;
    cJSON_AddBoolToObject(r, "ok", 1);
    for (i = 0; i < g_n_plugins; i++) {
        PluginRec* p = &g_plugins[i];
        cJSON* o = cJSON_CreateObject();
        char fpbuf[24];
        int j;
        cJSON* cmds;
        _snprintf(fpbuf, sizeof(fpbuf), "%016llx", p->fingerprint);
        /* NOTE: handle / dir / entry paths deliberately NOT exposed (docs\13
         * G6: handle leaves the API face; paths are host internals). */
        cJSON_AddStringToObject(o, "id", p->id);
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "version", p->version);
        if (p->current_version[0]) {
            /* b4 panel version column: pointer packages also carry the
             * on-disk version menu so the panel can offer switch/rollback
             * without touching the filesystem face. */
            char vlist[8][32];
            int nv = vm_list(p->dir, vlist, 8);
            int v;
            cJSON* av;
            cJSON_AddStringToObject(o, "currentVersion", p->current_version);
            av = cJSON_AddArrayToObject(o, "availableVersions");
            for (v = 0; v < nv; v++)
                cJSON_AddItemToArray(av, cJSON_CreateString(vlist[v]));
        }
        cJSON_AddStringToObject(o, "adapter", p->adapter);
        cJSON_AddStringToObject(o, "state", registry_state_name(p->state));
        cJSON_AddNumberToObject(o, "manifestVersion", p->manifest_version);
        cJSON_AddBoolToObject(o, "autoMount", p->auto_mount);
        cJSON_AddNumberToObject(o, "wantMount", p->want_known ? p->want_mount : -1);
        cJSON_AddStringToObject(o, "fingerprint", fpbuf);
        cJSON_AddBoolToObject(o, "fingerprintChanged", p->fingerprint_changed);
        if (p->state == PS_INVALID)
            cJSON_AddStringToObject(o, "invalidReason", p->invalid_reason);
        if (p->wait_reason[0])
            cJSON_AddStringToObject(o, "waitingFor", p->wait_reason);
        if (p->state == PS_ERROR || p->state == PS_TAINTED)
            cJSON_AddStringToObject(o, "lastErr", hub_errname(p->last_err));
        cmds = cJSON_AddArrayToObject(o, "commands");
        for (j = 0; j < p->n_commands && j < 16; j++)
            cJSON_AddItemToArray(cmds, cJSON_CreateString(p->commands[j]));
        add_str_array(o, "requires", p->requires_list, p->n_requires);
        add_str_array(o, "provides", p->provides_list, p->n_provides);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(r, "count", g_n_plugins);
    respond_cjson(conn, 200, r);
}

/* b4 (docs\23 §4.3): live confirmation tickets for the panel dialog.
 * Token-gated like every read; polled by the panel on the 2s tick.
 * Deliberately NOT audited (would flood the audit log at poll rate;
 * approve/deny themselves ARE audited at /confirm). */
static void get_pending(HttpdConn* conn)
{
    NoncePendingInfo infos[NONCE_SLOTS];
    int n = nonce_list_pending(infos, NONCE_SLOTS);
    cJSON* r = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(r, "pending");
    int i;
    cJSON_AddBoolToObject(r, "ok", 1);
    for (i = 0; i < n; i++) {
        cJSON* e = cJSON_CreateObject();
        char dhex[17];
        const char* pid = "?";
        const char* cid = "?";
        const char* title = "";
        if (infos[i].pidx >= 0 && infos[i].pidx < g_n_plugins) {
            PluginRec* p = &g_plugins[infos[i].pidx];
            pid = p->id;
            if (infos[i].cidx >= 0 && infos[i].cidx < p->n_commands) {
                cid = p->commands[infos[i].cidx];
                title = p->cmdv[infos[i].cidx].title;
            }
        }
        nonce_digest_hex(infos[i].digest, dhex);
        cJSON_AddStringToObject(e, "transactionId", infos[i].txn);
        cJSON_AddStringToObject(e, "nonceId", infos[i].nonce_id);
        cJSON_AddStringToObject(e, "plugin", pid);
        cJSON_AddStringToObject(e, "command", cid);
        cJSON_AddStringToObject(e, "title", title);
        cJSON_AddStringToObject(e, "risk", infos[i].risk);
        cJSON_AddStringToObject(e, "argsDigest", dhex);
        cJSON_AddStringToObject(e, "state", infos[i].approved ? "approved" : "issued");
        cJSON_AddNumberToObject(e, "remainingMs", (double)infos[i].remaining_ms);
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddNumberToObject(r, "count", n);
    respond_cjson(conn, 200, r);
}

static void get_manifest(HttpdConn* conn)
{
    /* G2: L3 single source of truth - full command contracts for the
     * bridge/agent face (M2.5) and the panel command view (G6). */
    cJSON* r = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(r, "plugins");
    int i;
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddStringToObject(r, "apiVersion", "v1");
    for (i = 0; i < g_n_plugins; i++) {
        PluginRec* p = &g_plugins[i];
        cJSON* o = cJSON_CreateObject();
        cJSON* cmds = cJSON_AddArrayToObject(o, "commands");
        char fpbuf[24];
        int j;
        _snprintf(fpbuf, sizeof(fpbuf), "%016llx", p->fingerprint);
        cJSON_AddStringToObject(o, "id", p->id);
        cJSON_AddStringToObject(o, "name", p->name);
        cJSON_AddStringToObject(o, "version", p->version);
        cJSON_AddNumberToObject(o, "manifestVersion", p->manifest_version);
        cJSON_AddStringToObject(o, "state", registry_state_name(p->state));
        cJSON_AddStringToObject(o, "fingerprint", fpbuf);
        cJSON_AddBoolToObject(o, "fingerprintChanged", p->fingerprint_changed);
        add_str_array(o, "requires", p->requires_list, p->n_requires);
        add_str_array(o, "provides", p->provides_list, p->n_provides);
        for (j = 0; j < p->n_commands && j < 16; j++) {
            cJSON* co = cJSON_CreateObject();
            cJSON_AddStringToObject(co, "id", p->commands[j]);
            if (p->manifest_version == 2) {
                PluginCmd* cv = &p->cmdv[j];
                cJSON* sch = cv->input_schema[0] ? cJSON_Parse(cv->input_schema) : NULL;
                cJSON_AddStringToObject(co, "title", cv->title);
                cJSON_AddStringToObject(co, "desc", cv->desc);
                cJSON_AddStringToObject(co, "descForLLM", cv->desc_llm);
                cJSON_AddStringToObject(co, "risk", cv->risk);
                cJSON_AddStringToObject(co, "context", cv->context[0] ? cv->context : "any");  /* M3b b1 */
                cJSON_AddNumberToObject(co, "reversible", cv->reversible);
                cJSON_AddNumberToObject(co, "idempotent", cv->idempotent);
                cJSON_AddNumberToObject(co, "timeoutSec", cv->timeout_sec);
                /* M4b additive contract fields (docs\39 3.1/3.5/3.6) */
                if (cv->is_async) cJSON_AddBoolToObject(co, "async", 1);
                if (cv->undo_cmd[0]) cJSON_AddStringToObject(co, "undoCommand", cv->undo_cmd);
                if (cv->group[0]) cJSON_AddStringToObject(co, "group", cv->group);
                cJSON_AddItemToObject(co, "inputSchema", sch ? sch : cJSON_CreateObject());
            }
            cJSON_AddItemToArray(cmds, co);
        }
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(r, "count", g_n_plugins);
    respond_cjson(conn, 200, r);
}

static void get_log(HttpdConn* conn, const HttpdRequest* req)
{
    /* GET /api/v1/log?offset=N -> incremental tail of logs\hub_m1.log.
     * offset is a BYTE offset; response carries the next offset. A partial
     * trailing line is never emitted (offset rewinds to the last \n). */
    long long off = 0, fsize, newoff;
    const char* q = req->query;
    char path[MAX_PATH + 48];
    FILE* f;
    int want, rd = 0;
    char* buf;
    cJSON* r;
    cJSON* lines;

    while (q && *q) {
        const char* amp = strchr(q, '&');
        char pair[128];
        int plen = amp ? (int)(amp - q) : (int)strlen(q);
        if (plen >= (int)sizeof(pair)) plen = (int)sizeof(pair) - 1;
        memcpy(pair, q, plen);
        pair[plen] = '\0';
        if (strncmp(pair, "offset=", 7) == 0) {
            const char* v = pair + 7;
            int i;
            if (!*v) { respond_err(conn, 400, "offset must be a non-negative integer"); return; }
            for (i = 0; v[i]; i++)
                if (!isdigit((unsigned char)v[i])) {
                    respond_err(conn, 400, "offset must be a non-negative integer");
                    return;
                }
            off = _atoi64(v);
        }
        /* unknown query keys ignored (forward compat) */
        q = amp ? amp + 1 : NULL;
    }

    _snprintf(path, sizeof(path), "%s\\logs\\hub_m1.log", g_root);
    f = fopen(path, "rb");
    if (!f) {
        r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddNumberToObject(r, "offset", 0);
        cJSON_AddNumberToObject(r, "size", 0);
        cJSON_AddArrayToObject(r, "lines");
        respond_cjson(conn, 200, r);
        return;
    }
    fseek(f, 0, SEEK_END);
    fsize = _ftelli64(f);
    if (off > fsize) off = 0;          /* file rotated/truncated: restart */
    _fseeki64(f, off, SEEK_SET);
    want = (int)((fsize - off > LOG_TAIL_CAP) ? LOG_TAIL_CAP : (fsize - off));
    buf = (char*)malloc((size_t)want + 1);
    if (!buf) { fclose(f); respond_err(conn, 500, "oom"); return; }
    if (want > 0) rd = (int)fread(buf, 1, (size_t)want, f);
    fclose(f);
    buf[rd] = '\0';
    if (rd > 0 && buf[rd - 1] != '\n') {
        char* ln = strrchr(buf, '\n');
        rd = ln ? (int)(ln + 1 - buf) : 0;   /* hold back partial line */
        buf[rd] = '\0';
    }
    newoff = off + rd;

    r = cJSON_CreateObject();
    lines = cJSON_AddArrayToObject(r, "lines");
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddNumberToObject(r, "offset", (double)newoff);
    cJSON_AddNumberToObject(r, "size", (double)fsize);
    {
        char* cur = buf;
        while (cur < buf + rd) {
            char* nl = strchr(cur, '\n');
            int len;
            if (!nl) break;
            len = (int)(nl - cur);
            if (len > 0 && cur[len - 1] == '\r') len--;
            if (len > 0) {
                char save = cur[len];
                cur[len] = '\0';
                cJSON_AddItemToArray(lines, cJSON_CreateString(cur));
                cur[len] = save;
            }
            cur = nl + 1;
        }
    }
    free(buf);
    respond_cjson(conn, 200, r);
}

/* ------------------------------------------------------------ G1 queue */

/* submit: returns slot index, -1 queue full (503 busy), -2 executor not
 * ready (503 hub not ready). seq_out receives the envelope seq. */
static int queue_submit(const char* op_type, const char* payload,
                        int timeout_ms, long* seq_out)
{
    int i, found = -1;
    WebOpSlot* s;
    if (!g_executor_ready) return -2;
    if (g_stopping) return -2;      /* terminate window: reject fast */
    EnterCriticalSection(&g_cs);
    for (i = 0; i < Q_SLOTS; i++) {
        if (!g_q[i].used) { found = i; break; }
    }
    if (found < 0) { LeaveCriticalSection(&g_cs); return -1; }
    s = &g_q[found];
    s->used = 1;
    s->taken = 0;
    s->executed = 0;
    s->abandoned = 0;
    s->started = 0;
    s->status = 200;
    s->result_json[0] = '\0';
    s->plugin_id[0] = '\0';
    s->result_state[0] = '\0';
    ResetEvent(s->event);
    strncpy(s->op_type, op_type, sizeof(s->op_type) - 1);
    s->op_type[sizeof(s->op_type) - 1] = '\0';
    strncpy(s->payload_json, payload ? payload : "", sizeof(s->payload_json) - 1);
    s->payload_json[sizeof(s->payload_json) - 1] = '\0';
    s->timeout_ms = timeout_ms;
    s->seq = ++g_seq;
    *seq_out = s->seq;
    LeaveCriticalSection(&g_cs);
    return found;
}

static void queue_release(int slot)
{
    EnterCriticalSection(&g_cs);
    g_q[slot].used = 0;
    LeaveCriticalSection(&g_cs);
}

/* ------------------------------------------------------------ executor */

/* Business execution - runs ONLY on the executor thread. Fills out (JSON
 * text), idout, stateout (audit fields); returns HTTP status (always 200:
 * business failures are 200 + ok:false per docs\09 4). */
static int execute_op(const char* op, const char* payload,
                      char* out, int outsz, char* idout, char* stateout)
{
    cJSON* body;
    cJSON* jid;
    cJSON* jval;
    int idx;

    idout[0] = '\0';
    stateout[0] = '\0';

#ifdef CREO_HUB_DEV
    /* M3a: dev experiment executor - runs HERE (executor thread = Creo main
     * thread) through the same G1 envelope path future /call will use.
     * Production build: compiled out (L17: no "_e11" string in prod DLL). */
    if (strcmp(op, "_e11") == 0)
        return dev_e11_execute(payload, out, outsz, idout, stateout);
#endif

    if (strcmp(op, "call") == 0)
        return call_exec_execute(payload, out, outsz, idout, stateout);

    if (strcmp(op, "refresh") == 0) {
        int n, i, active = 0, invalid = 0;
        n = registry_rescan(g_root);
        for (i = 0; i < g_n_plugins; i++) {
            if (g_plugins[i].state == PS_ACTIVE) active++;
            if (g_plugins[i].state == PS_INVALID) invalid++;
        }
        _snprintf(out, outsz,
                  "{\"ok\":true,\"total\":%d,\"active\":%d,\"invalid\":%d}",
                  n, active, invalid);
        strcpy(idout, "*");
        _snprintf(stateout, 96, "total=%d active=%d invalid=%d", n, active, invalid);
        return 200;
    }
    if (strcmp(op, "mount-all") == 0 || strcmp(op, "unload-all") == 0) {
        ReconcileResult rr;
        int i, rc;
        memset(&rr, 0, sizeof(rr));
        if (strcmp(op, "mount-all") == 0) {
            for (i = 0; i < g_n_plugins; i++) {
                PluginState st = g_plugins[i].state;
                /* include ACTIVE so mount-all never surprise-unmounts a
                 * record whose target says 0 (e.g. panel-unmounted then
                 * menu-mounted): after mount-all, target == committed. */
                if (st == PS_REGISTERED || st == PS_ERROR || st == PS_ACTIVE)
                    reconcile_set_target(i, 1);
            }
        } else {
            /* unload-all = "nothing mounted": clear targets on every
             * actionable record so reconcile does not retry ERROR ones. */
            for (i = 0; i < g_n_plugins; i++) {
                PluginState st = g_plugins[i].state;
                if (st != PS_INVALID && st != PS_UNSUPPORTED)
                    reconcile_set_target(i, 0);
            }
        }
        rc = reconcile_run(RECONCILE_ORDER_TOPO, 1, &rr);
        strcpy(idout, "*");
        if (rc != 0) {
            _snprintf(out, outsz, "{\"ok\":false,\"error\":\"reconcile rejected order strategy\"}");
            strcpy(stateout, "rc=-1");
            return 200;
        }
        _snprintf(out, outsz,
                  "{\"ok\":true,\"mounted\":%d,\"unmounted\":%d,\"failed\":%d,\"skipped\":%d}",
                  rr.mounted, rr.unmounted, rr.failed, rr.skipped);
        _snprintf(stateout, 96, "m=%d u=%d f=%d s=%d",
                  rr.mounted, rr.unmounted, rr.failed, rr.skipped);
        return 200;
    }

    /* id-carrying ops */
    body = cJSON_Parse(payload);
    if (!body || !cJSON_IsObject(body)) {
        cJSON_Delete(body);
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"body must be a JSON object\"}");
        strcpy(stateout, "bad-body");
        return 200;
    }
    if (strcmp(op, "opmode") == 0) {
        /* M3e Q1 operator mode menu (user-ratified): no plugin id; armed by
         * a human from the panel (same-user loopback token trust boundary,
         * guide §8.4.4). M2 demands explicit ack + whitelisted window. */
        cJSON* jm = cJSON_GetObjectItemCaseSensitive(body, "mode");
        cJSON* jw = cJSON_GetObjectItemCaseSensitive(body, "minutes");
        cJSON* ja = cJSON_GetObjectItemCaseSensitive(body, "ack");
        int mode = cJSON_IsNumber(jm) ? jm->valueint : -1;
        unsigned long long win = 0;
        int ack = (ja && cJSON_IsTrue(ja)) ? 1 : 0;
        int mout = 0;
        unsigned long long rem = 0;
        if (mode == OPMODE_UNATTENDED) {
            int mins = cJSON_IsNumber(jw) ? jw->valueint : 10;
            if ((mins != 5 && mins != 10 && mins != 30) || !ack) {
                _snprintf(out, outsz,
                          "{\"ok\":false,\"error\":\"mode 2 requires minutes in {5,10,30} and ack:true\"}");
                strcpy(stateout, "bad-args");
                cJSON_Delete(body);
                return 400;
            }
            win = (unsigned long long)mins * 60000ULL;
        }
        if (opmode_set(mode, win, ack) != 0) {
            _snprintf(out, outsz, "{\"ok\":false,\"error\":\"mode must be 0, 1 or 2\"}");
            strcpy(stateout, "bad-args");
            cJSON_Delete(body);
            return 400;
        }
        opmode_get(&mout, &rem);
        _snprintf(out, outsz, "{\"ok\":true,\"mode\":%d,\"remainMs\":%llu}", mout, rem);
        _snprintf(stateout, 96, "mode=%d remain=%llu", mout, rem);
        cJSON_Delete(body);
        return 200;
    }
    jid = cJSON_GetObjectItemCaseSensitive(body, "id");
    if (!cJSON_IsString(jid) || !jid->valuestring[0]) {
        cJSON_Delete(body);
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"id required (non-empty string)\"}");
        strcpy(stateout, "bad-args");
        return 200;
    }
    strncpy(idout, jid->valuestring, 63);
    idout[63] = '\0';
    idx = registry_find_idx(jid->valuestring);
    if (idx < 0) {
        cJSON_Delete(body);
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"plugin not found\"}");
        strcpy(stateout, "not-found");
        return 200;
    }

    if (strcmp(op, "mount") == 0 || strcmp(op, "unmount") == 0) {
        ProError e;
        PluginState st;
        int is_mount = (strcmp(op, "mount") == 0);
        int okf;
        if (!is_mount) {
            /* b3 drain-style unmount (docs\22 8.1 / docs\23 3.5): dependents
             * first (reverse topo), target last; dryRun returns the plan
             * without executing (additive field drainedList). */
            int order[MAX_PLUGINS], n, k;
            char plan[512];
            cJSON* dry = cJSON_GetObjectItem(body, "dryRun");
            n = registry_drain_plan(idx, order, MAX_PLUGINS);
            plan[0] = '\0';
            for (k = 0; k < n; k++) {
                char one[80];
                _snprintf(one, sizeof(one), "%s\"%s\"", k ? "," : "",
                          g_plugins[order[k]].id);
                one[sizeof(one) - 1] = '\0';
                if (strlen(plan) + strlen(one) < sizeof(plan) - 1) strcat(plan, one);
            }
            if (dry && cJSON_IsTrue(dry)) {
                _snprintf(out, outsz,
                          "{\"ok\":true,\"dryRun\":true,\"drainedList\":[%s]}", plan);
                strcpy(stateout, "drain-plan");
                cJSON_Delete(body);
                return 200;
            }
            for (k = 0; k < n; k++) reconcile_set_target(order[k], 0);
            e = registry_unmount_drain(idx, 1);
            st = g_plugins[idx].state;
            okf = (st == PS_REGISTERED || st == PS_ERROR);
            _snprintf(stateout, 96, "%s err=%s", registry_state_name(st), hub_errname(e));
            if (okf)
                _snprintf(out, outsz,
                          "{\"ok\":true,\"state\":\"%s\",\"drainedList\":[%s]}",
                          registry_state_name(st), plan);
            else
                _snprintf(out, outsz,
                          "{\"ok\":false,\"error\":\"unmount failed\",\"code\":\"%s\","
                          "\"state\":\"%s\",\"drainedList\":[%s]}",
                          hub_errname(e), registry_state_name(st), plan);
            cJSON_Delete(body);
            return 200;
        }
        /* G3: explicit target edit, then the guarded op converges THIS
         * record (single-record convergence; reconcile_run stays for
         * bulk/startup paths so one button never side-effects others). */
        reconcile_set_target(idx, 1);
        e = registry_mount(idx, 1);
        st = g_plugins[idx].state;
        /* success semantics: mount ok iff ACTIVE. INVALID/UNSUPPORTED/TAINTED
         * stay ok:false (honest: nothing converged). */
        okf = (st == PS_ACTIVE);
        _snprintf(stateout, 96, "%s err=%s", registry_state_name(st), hub_errname(e));
        if (okf)
            _snprintf(out, outsz, "{\"ok\":true,\"state\":\"%s\"}", registry_state_name(st));
        else
            _snprintf(out, outsz,
                      "{\"ok\":false,\"error\":\"%s failed\",\"code\":\"%s\",\"state\":\"%s\"}",
                      op, hub_errname(e), registry_state_name(st));
        cJSON_Delete(body);
        return 200;
    }
    if (strcmp(op, "update") == 0 || strcmp(op, "rollback") == 0) {
        /* b3 P1 (docs\23 3.6): pointer switch + remount. Failure preserves
         * the scene (P1b manual rollback): a failed remount leaves state
         * ERROR with the new pointer and an honest ok:false. */
        char ver[32], err[128];
        char idcopy[64];
        ProError e = PRO_TK_NO_ERROR;
        PluginState st;
        int okf, was_active;
        cJSON* jv = cJSON_GetObjectItem(body, "version");
        if (!cJSON_IsString(jv) || !jv->valuestring[0]) {
            _snprintf(out, outsz,
                      "{\"ok\":false,\"error\":\"version (non-empty string) required\"}");
            strcpy(stateout, "bad-version");
            cJSON_Delete(body);
            return 200;
        }
        strncpy(ver, jv->valuestring, sizeof(ver) - 1);
        ver[sizeof(ver) - 1] = '\0';
        if (!vm_has(g_plugins[idx].dir, ver)) {
            _snprintf(out, outsz,
                      "{\"ok\":false,\"error\":\"version dir missing\","
                      "\"version\":\"%s\"}", ver);
            strcpy(stateout, "version-missing");
            cJSON_Delete(body);
            return 200;
        }
        err[0] = '\0';
        if (!vm_set_current(g_plugins[idx].dir, ver, err, sizeof(err))) {
            _snprintf(out, outsz,
                      "{\"ok\":false,\"error\":\"pointer switch failed\",\"detail\":\"%s\"}",
                      err[0] ? err : "?");
            strcpy(stateout, "pointer-fail");
            cJSON_Delete(body);
            return 200;
        }
        strncpy(idcopy, g_plugins[idx].id, sizeof(idcopy) - 1);
        idcopy[sizeof(idcopy) - 1] = '\0';
        registry_rescan(g_root);
        idx = registry_find_idx(idcopy);
        was_active = (idx >= 0 && g_plugins[idx].state == PS_ACTIVE);
        if (idx >= 0 && was_active) {
            registry_unmount(idx, 1);
            e = registry_mount(idx, 1);
        }
        st = (idx >= 0) ? g_plugins[idx].state : PS_INVALID;
        okf = (idx >= 0) && (was_active ? (st == PS_ACTIVE)
                                        : (st == PS_REGISTERED || st == PS_ERROR));
        _snprintf(stateout, 96, "%s err=%s", registry_state_name(st), hub_errname(e));
        if (okf)
            _snprintf(out, outsz,
                      "{\"ok\":true,\"state\":\"%s\",\"currentVersion\":\"%s\"}",
                      registry_state_name(st), ver);
        else
            _snprintf(out, outsz,
                      "{\"ok\":false,\"error\":\"%s remount failed\",\"code\":\"%s\","
                      "\"state\":\"%s\",\"currentVersion\":\"%s\","
                      "\"hint\":\"scene preserved; POST rollback to revert\"}",
                      op, hub_errname(e), registry_state_name(st), ver);
        cJSON_Delete(body);
        return 200;
    }
    if (strcmp(op, "automount") == 0) {
        jval = cJSON_GetObjectItemCaseSensitive(body, "value");
        if (!cJSON_IsBool(jval)) {
            cJSON_Delete(body);
            _snprintf(out, outsz, "{\"ok\":false,\"error\":\"value required (boolean)\"}");
            strcpy(stateout, "bad-args");
            return 200;
        }
        registry_set_automount(idx, cJSON_IsTrue(jval) ? 1 : 0);
        _snprintf(out, outsz, "{\"ok\":true,\"autoMount\":%s,\"state\":\"%s\"}",
                  cJSON_IsTrue(jval) ? "true" : "false",
                  registry_state_name(g_plugins[idx].state));
        _snprintf(stateout, 96, "autoMount=%s", cJSON_IsTrue(jval) ? "true" : "false");
        cJSON_Delete(body);
        return 200;
    }

    cJSON_Delete(body);
    _snprintf(out, outsz, "{\"ok\":false,\"error\":\"unknown op\"}");
    strcpy(stateout, "unknown-op");
    return 200;
}

int web_api_run_pending(void)
{
    int i, pick = -1;
    long best = 0;
    WebOpSlot* s;
    char op[32], payload[PAYLOAD_CAP], res[RESULT_CAP], idout[64], stateout[160];
    long seq;
    int status;
    ULONGLONG e0;
    cJSON* rj;
    char* stamped;
    int job_idx;
    char job_id[16];

    if (!g_inited) return 0;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < Q_SLOTS; i++) {
        if (g_q[i].used && !g_q[i].executed && !g_q[i].taken) {
            if (pick < 0 || g_q[i].seq < best) { pick = i; best = g_q[i].seq; }
        }
    }
    if (pick < 0) { LeaveCriticalSection(&g_cs); return 0; }
    s = &g_q[pick];
    s->taken = 1;
    /* abandoned envelopes are DROPPED without executing: the caller already
     * reported timeout; late side effects would violate least surprise.
     * Retry is the user's explicit decision (audited as DROPPED). */
    if (s->abandoned) {
        strncpy(op, s->op_type, sizeof(op) - 1); op[sizeof(op) - 1] = '\0';
        strncpy(payload, s->payload_json, sizeof(payload) - 1); payload[sizeof(payload) - 1] = '\0';
        seq = s->seq;
        s->used = 0;
        LeaveCriticalSection(&g_cs);
        audit(op, "-", payload, "DROPPED(abandoned after timeout)", 0);
        (void)seq;
        return 1;
    }
    strncpy(op, s->op_type, sizeof(op) - 1); op[sizeof(op) - 1] = '\0';
    strncpy(payload, s->payload_json, sizeof(payload) - 1); payload[sizeof(payload) - 1] = '\0';
    seq = s->seq;
    s->started = 1;   /* b2 orphan protocol: execution begins now */
    LeaveCriticalSection(&g_cs);

    /* M4b: async envelope? (post_op injected "_job"/"_job_slot") */
    job_idx = -1;
    job_id[0] = '\0';
    {
        cJSON* jb = cJSON_Parse(payload);
        if (jb) {
            cJSON* jj = cJSON_GetObjectItemCaseSensitive(jb, "_job");
            if (cJSON_IsString(jj) && jj->valuestring[0]) {
                strncpy(job_id, jj->valuestring, sizeof(job_id) - 1);
                job_idx = jobs_find(job_id);
            }
            cJSON_Delete(jb);
        }
    }
    if (job_idx >= 0) jobs_mark_working(job_idx);

    e0 = GetTickCount64();
    status = execute_op(op, payload, res, sizeof(res), idout, stateout);

    /* G1: stamp seq+op into every result envelope */
    rj = cJSON_Parse(res);
    if (rj) {
        cJSON_AddNumberToObject(rj, "seq", (double)seq);
        cJSON_AddStringToObject(rj, "op", op);
        stamped = cJSON_PrintUnformatted(rj);
        cJSON_Delete(rj);
        if (stamped) {
            strncpy(res, stamped, sizeof(res) - 1);
            res[sizeof(res) - 1] = '\0';
            cJSON_free(stamped);
        }
    }

    EnterCriticalSection(&g_cs);
    s->status = status;
    strncpy(s->result_json, res, sizeof(s->result_json) - 1);
    s->result_json[sizeof(s->result_json) - 1] = '\0';
    strncpy(s->plugin_id, idout, sizeof(s->plugin_id) - 1);
    strncpy(s->result_state, stateout, sizeof(s->result_state) - 1);
    s->executed = 1;
    if (job_idx >= 0) {
        /* async completion: write the job row, free the envelope slot (nobody
         * waits on the event), and audit the real outcome (R4 evidence). */
        int ok = (status == 200) && (strstr(res, "\"ok\":false") == NULL);
        jobs_finish(job_idx, ok, res, stateout);
        s->used = 0;
        LeaveCriticalSection(&g_cs);
        audit(op, idout, payload, stateout, (DWORD)(GetTickCount64() - e0));
        return 1;
    }
    if (s->abandoned) {
        s->used = 0;      /* worker left between execute and here */
        LeaveCriticalSection(&g_cs);
        if (strcmp(op, "call") == 0) {
            /* b2 orphan protocol (§6.4): the 504 already went out; this is
             * the late completion - audited as op=call.late, orphan flag
             * cleared so new /call dispatch is accepted again. */
            InterlockedExchange(&g_orphaned, 0);
            audit("call.late", idout, payload, stateout, (DWORD)(GetTickCount64() - e0));
        } else {
            audit(op, idout, payload, "DROPPED(abandoned during execute)", 0);
        }
        return 1;
    }
    SetEvent(s->event);
    LeaveCriticalSection(&g_cs);
    return 1;
}

/* ------------------------------------------------------------ POST path */

static void post_op(HttpdConn* conn, const HttpdRequest* req,
                    const char* op_type, DWORD t0)
{
    cJSON* body;
    cJSON* tov;
    int timeout_ms = DEFAULT_TIMEOUT_MS;
    int slot;
    long seq = 0;
    DWORD wr;
    CallTarget tgt;              /* hoisted: the 504 branch needs idempotent */
    int is_call;
    int was_started = 0;
    memset(&tgt, 0, sizeof(tgt));
    is_call = (strcmp(op_type, "call") == 0);

    /* L4: body must exist and be JSON (CT already checked by caller) */
    if (!req->body || !req->body[0]) {
        audit(op_type, "-", "", "REJECT(empty body)", GetTickCount() - t0);
        respond_err(conn, 400, "body must be a JSON object");
        return;
    }
    body = cJSON_Parse(req->body);
    if (!body || !cJSON_IsObject(body)) {
        cJSON_Delete(body);
        audit(op_type, "-", req->body, "REJECT(json parse)", GetTickCount() - t0);
        respond_err(conn, 400, "body must be a JSON object");
        return;
    }
    tov = cJSON_GetObjectItemCaseSensitive(body, "timeoutMs");
    if (tov) {
        if (!cJSON_IsNumber(tov) || tov->valueint < 1 || tov->valueint > MAX_TIMEOUT_MS) {
            cJSON_Delete(body);
            audit(op_type, "-", req->body, "REJECT(timeoutMs)", GetTickCount() - t0);
            respond_err(conn, 400, "timeoutMs must be a number in [1,30000]");
            return;
        }
        timeout_ms = tov->valueint;
    }
    /* b2: /api/v1/confirm is handled INLINE (never queued): approvals must
     * keep working while the pump is busy or orphaned, and the nonce table
     * owns its own critical section (docs\22 §6.3). */
    if (strcmp(op_type, "confirm") == 0) {
        const cJSON* jtxn = cJSON_GetObjectItemCaseSensitive(body, "transactionId");
        const cJSON* jap  = cJSON_GetObjectItemCaseSensitive(body, "approve");
        char txnb[64];
        char rbuf[320];
        char* q;
        NonceResult nr;
        int approve;
        if (!jtxn || !cJSON_IsString(jtxn) || !jap || !cJSON_IsBool(jap)) {
            cJSON_Delete(body);
            audit("confirm", "-", req->body, "REJECT(shape)", GetTickCount() - t0);
            respond_err(conn, 400, "body: {transactionId:<from 428 needConfirm>, approve:true|false}");
            return;
        }
        strncpy(txnb, jtxn->valuestring, sizeof(txnb) - 1);
        txnb[sizeof(txnb) - 1] = '\0';
        for (q = txnb; *q; q++)                    /* JSON-injection scrub */
            if (!isalnum((unsigned char)*q)) *q = '?';
        approve = cJSON_IsTrue(jap);
        cJSON_Delete(body);
        nr = approve ? nonce_approve_txn(txnb) : nonce_deny_txn(txnb);
        if (nr == NR_OK) {
            if (approve) {
                _snprintf(rbuf, sizeof(rbuf),
                    "{\"ok\":true,\"transactionId\":\"%s\",\"state\":\"APPROVED\","
                    "\"hint\":\"retry the /call now: with confirm_nonce, or without it "
                    "(auto-pair on identical args within 60s)\"}", txnb);
                audit("confirm_approve", "-", txnb, "APPROVED", GetTickCount() - t0);
            } else {
                _snprintf(rbuf, sizeof(rbuf),
                    "{\"ok\":true,\"transactionId\":\"%s\",\"state\":\"DENIED\"}", txnb);
                audit("confirm_deny", "-", txnb, "DENIED(slot freed)", GetTickCount() - t0);
            }
            httpd_respond_json(conn, 200, rbuf);
        } else if (nr == NR_EXPIRED) {
            _snprintf(rbuf, sizeof(rbuf),
                "{\"ok\":false,\"error\":\"NONCE_EXPIRED\",\"transactionId\":\"%s\","
                "\"hint\":\"confirmation TTL (60s) over; re-issue via /call\"}", txnb);
            audit(approve ? "confirm_approve" : "confirm_deny", "-", txnb, "REJECT(expired)", GetTickCount() - t0);
            httpd_respond_json(conn, 409, rbuf);
        } else if (nr == NR_CONSUMED) {
            _snprintf(rbuf, sizeof(rbuf),
                "{\"ok\":false,\"error\":\"NONCE_CONSUMED\",\"transactionId\":\"%s\"}", txnb);
            audit(approve ? "confirm_approve" : "confirm_deny", "-", txnb, "REJECT(consumed)", GetTickCount() - t0);
            httpd_respond_json(conn, 409, rbuf);
        } else {
            _snprintf(rbuf, sizeof(rbuf),
                "{\"ok\":false,\"error\":\"transaction not found\",\"transactionId\":\"%s\","
                "\"hint\":\"unknown, evicted, or already denied transactionId\"}", txnb);
            audit(approve ? "confirm_approve" : "confirm_deny", "-", txnb, "REJECT(not found)", GetTickCount() - t0);
            httpd_respond_json(conn, 404, rbuf);
        }
        return;
    }
    if (is_call) {
        /* M3b b1: full fast-fail validation chain on the worker thread
         * (docs\22 §6.1: fast-fails never touch the envelope queue). */
        int pre_status = 0;
        char resp[RESULT_CAP];
        /* b2 orphan gate (§6.4): while a timed-out command still owns the
         * Creo main thread, every new /call dispatch is refused fast. */
        if (InterlockedCompareExchange(&g_orphaned, 1, 1)) {
            cJSON_Delete(body);
            audit(op_type, "-", req->body, "REJECT(busy-orphaned)", GetTickCount() - t0);
            httpd_respond_json(conn, 503,
                "{\"ok\":false,\"error\":\"hub busy-orphaned\",\"retryAfterMs\":1000,"
                "\"hint\":\"a timed-out command still owns the Creo main thread; "
                "wait for op=call.late in the log, then retry\"}");
            return;
        }
        if (!call_exec_precheck(body, &tgt, resp, sizeof(resp), &pre_status)) {
            char rbuf[64];
            cJSON_Delete(body);
            if (pre_status == 200)
                strcpy(rbuf, "DRYRUN(not enqueued)");
            else
                _snprintf(rbuf, sizeof(rbuf), "REJECT(precheck %d)", pre_status);
            rbuf[sizeof(rbuf) - 1] = '\0';
            audit(op_type, "-", req->body, rbuf, GetTickCount() - t0);
            httpd_respond_json(conn, pre_status, resp);
            return;
        }
        /* no explicit client timeoutMs -> envelope wait = command budget + 500ms */
        if (!tov) timeout_ms = tgt.timeout_ms + 500;

        /* M4b async: answer 202 + jobId right away and let the executor run
         * the command later on the main thread. The httpd worker stays free
         * to serve GET /api/v1/jobs/<id> while the command runs (docs\39 3.1). */
        if (tgt.is_async) {
            char jid[16], jslot[96];
            int jidx;
            char* aug;
            jidx = jobs_create(tgt.plugin, tgt.command, tgt.risk, tgt.timeout_ms,
                               jid, sizeof(jid), jslot, sizeof(jslot));
            if (jidx < 0) {
                cJSON_Delete(body);
                audit(op_type, "-", req->body, "REJECT(jobs full)", GetTickCount() - t0);
                httpd_respond_json(conn, 503,
                    "{\"ok\":false,\"error\":\"jobs full\",\"retryAfterMs\":1000,"
                    "\"hint\":\"all async job slots are live; wait for one to finish\"}");
                return;
            }
            cJSON_AddStringToObject(body, "_job", jid);
            if (jslot[0]) cJSON_AddStringToObject(body, "_job_slot", jslot);
            aug = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            if (!aug) {
                jobs_cancel(jidx, "payload");
                audit(op_type, "-", req->body, "REJECT(async payload)", GetTickCount() - t0);
                httpd_respond_json(conn, 500, "{\"ok\":false,\"error\":\"async payload build failed\"}");
                return;
            }
            slot = queue_submit(op_type, aug, tgt.timeout_ms + 1000, &seq);
            cJSON_free(aug);
            if (slot < 0) {
                jobs_cancel(jidx, slot == -1 ? "queue full" : "hub not ready");
                audit(op_type, "-", req->body, "REJECT(async queue)", GetTickCount() - t0);
                httpd_respond_json(conn, 503, slot == -1
                    ? "{\"ok\":false,\"error\":\"busy\",\"retryAfterMs\":500}"
                    : "{\"ok\":false,\"error\":\"hub not ready\"}");
                return;
            }
            if (g_wakeup) g_wakeup();
            {
                char buf[512];
                _snprintf(buf, sizeof(buf),
                    "{\"ok\":true,\"result\":{\"jobId\":\"%s\"},"
                    "\"poll\":{\"endpoint\":\"/api/v1/jobs/%s\",\"intervalMs\":500},"
                    "\"meta\":{\"async\":true,\"etaMs\":%d},"
                    "\"op\":\"call\",\"seq\":%ld}",
                    jid, jid, tgt.timeout_ms, seq);
                buf[sizeof(buf) - 1] = '\0';
                audit(op_type, tgt.plugin, req->body, "ACCEPTED(async)", GetTickCount() - t0);
                httpd_respond_json(conn, 202, buf);
            }
            return;
        }
    }
    cJSON_Delete(body);

    slot = queue_submit(op_type, req->body, timeout_ms, &seq);
    if (slot == -2) {
        audit(op_type, "-", req->body, "REJECT(hub not ready)", GetTickCount() - t0);
        httpd_respond_json(conn, 503, "{\"ok\":false,\"error\":\"hub not ready\"}");
        return;
    }
    if (slot == -1) {
        audit(op_type, "-", req->body, "REJECT(busy)", GetTickCount() - t0);
        httpd_respond_json(conn, 503, "{\"ok\":false,\"error\":\"busy\",\"retryAfterMs\":500}");
        return;
    }
    /* E6: wake the main thread immediately (fallback: 500ms ProUITimer) */
    if (g_wakeup) g_wakeup();

    wr = WaitForSingleObject(g_q[slot].event, (DWORD)timeout_ms);
    if (wr == WAIT_OBJECT_0) {
        int status;
        char result[RESULT_CAP], pid[64], rstate[96];
        EnterCriticalSection(&g_cs);
        if (g_stopping && !g_q[slot].executed) {
            /* woken by terminate phase 1 (web_api_stop_waiters): this envelope
             * will never execute - answer fast so httpd_stop() can join. If the
             * executor DID finish first (race), fall through and honor result. */
            g_q[slot].used = 0;
            LeaveCriticalSection(&g_cs);
            audit(op_type, "-", req->body, "REJECT(hub stopping)", GetTickCount() - t0);
            httpd_respond_json(conn, 503, "{\"ok\":false,\"error\":\"hub stopping\"}");
            return;
        }
        status = g_q[slot].status;
        strncpy(result, g_q[slot].result_json, sizeof(result) - 1);
        result[sizeof(result) - 1] = '\0';
        strncpy(pid, g_q[slot].plugin_id, sizeof(pid) - 1);
        pid[sizeof(pid) - 1] = '\0';
        strncpy(rstate, g_q[slot].result_state, sizeof(rstate) - 1);
        rstate[sizeof(rstate) - 1] = '\0';
        g_q[slot].used = 0;
        LeaveCriticalSection(&g_cs);
        audit(op_type, pid, req->body, rstate, GetTickCount() - t0);
        httpd_respond_json(conn, status, result);
        return;
    }

    /* timeout: abandon; executor drops the envelope later (audited) */
    EnterCriticalSection(&g_cs);
    g_q[slot].abandoned = 1;
    if (g_q[slot].executed) {        /* completed between timeout and CS: honor the result anyway */
        int status = g_q[slot].status;
        char result[RESULT_CAP], pid[64], rstate[96];
        strncpy(result, g_q[slot].result_json, sizeof(result) - 1);
        result[sizeof(result) - 1] = '\0';
        strncpy(pid, g_q[slot].plugin_id, sizeof(pid) - 1);
        pid[sizeof(pid) - 1] = '\0';
        strncpy(rstate, g_q[slot].result_state, sizeof(rstate) - 1);
        rstate[sizeof(rstate) - 1] = '\0';
        g_q[slot].used = 0;
        LeaveCriticalSection(&g_cs);
        audit(op_type, pid, req->body, rstate, GetTickCount() - t0);
        httpd_respond_json(conn, status, result);
        return;
    }
    was_started = g_q[slot].started;
    LeaveCriticalSection(&g_cs);
    if (is_call) {
        /* b2 three-layer timeout contract (docs\22 §6.4): inner queue
         * timeout = cancelled-before-start (never executed, safe retry);
         * post-start timeout = 504 report-don't-kill + orphan protocol. */
        char buf[512];
        if (!was_started) {
            _snprintf(buf, sizeof(buf),
                      "{\"ok\":false,\"error\":\"deadline exceeded\",\"retryable\":true,"
                      "\"meta\":{\"timedOut\":true,\"cancelledBeforeStart\":true,"
                      "\"warning\":\"cancelled while queued - never executed; safe to retry\"},"
                      "\"op\":\"%s\",\"seq\":%ld,\"timeoutMs\":%d}",
                      op_type, seq, timeout_ms);
            audit(op_type, "-", req->body, "TIMEOUT(cancelled-before-start)", GetTickCount() - t0);
        } else {
            InterlockedExchange(&g_orphaned, 1);
            _snprintf(buf, sizeof(buf),
                      "{\"ok\":false,\"error\":\"deadline exceeded\",\"retryable\":%s,"
                      "\"meta\":{\"timedOut\":true,\"warning\":\"command may still be running "
                      "or have completed - check state/health before any retry\"},"
                      "\"op\":\"%s\",\"seq\":%ld,\"timeoutMs\":%d}",
                      tgt.idempotent ? "true" : "false", op_type, seq, timeout_ms);
            audit(op_type, "-", req->body, "TIMEOUT(orphaned)", GetTickCount() - t0);
        }
        httpd_respond_json(conn, 504, buf);
        return;
    }
    {
        char buf[256];
        _snprintf(buf, sizeof(buf),
                  "{\"ok\":false,\"error\":\"timeout\",\"op\":\"%s\",\"seq\":%ld,\"timeoutMs\":%d}",
                  op_type, seq, timeout_ms);
        audit(op_type, "-", req->body, "TIMEOUT", GetTickCount() - t0);
        httpd_respond_json(conn, 200, buf);
    }
}

/* ------------------------------------------------------------ router */

int web_api_route(const HttpdRequest* req, HttpdConn* conn)
{
    static const char* GET_ROUTES[6] = {
        "/api/v1/health", "/api/v1/plugins", "/api/v1/manifest", "/api/v1/log",
        "/api/v1/pending",  /* b4: confirm-dialog feed (docs\23 §4.3) */
        "/api/v1/jobs"      /* M4b: async job table (docs\39 3.4) */
    };
    static const struct { const char* path; const char* op; } POST_ROUTES[] = {
        { "/api/v1/mount",      "mount" },
        { "/api/v1/unmount",    "unmount" },
        { "/api/v1/automount",  "automount" },
        { "/api/v1/refresh",    "refresh" },
        { "/api/v1/mount-all",  "mount-all" },
        { "/api/v1/unload-all", "unload-all" },
        { "/api/v1/call",       "call" },        /* M3b b1 (docs\22 §6.1) */
        { "/api/v1/confirm",    "confirm" },     /* M3b b2 (§6.3 approval registration) */
        { "/api/v1/update",     "update" },      /* M3b b3 P1 (pointer switch + remount) */
        { "/api/v1/rollback",   "rollback" },    /* M3b b3 P1b manual rollback */
        { "/api/v1/opmode",     "opmode" }       /* M3e Q1 operator mode menu (no plugin id) */
#ifdef CREO_HUB_DEV
        , { "/api/v1/_e11",     "_e11" }    /* M3a experiment endpoint (dev builds only) */
#endif
    };
#define N_POST_ROUTES ((int)(sizeof(POST_ROUTES) / sizeof(POST_ROUTES[0])))
    DWORD t0 = GetTickCount();
    const char* p = req->path;
    int i;
    char opname[80];

    if (!g_inited) {
        respond_err(conn, 500, "api not initialized");
        return 1;
    }

    /* L3: token gate over the WHOLE api face (reads included) */
    if (!g_token[0] || strcmp(req->token, g_token) != 0) {
        _snprintf(opname, sizeof(opname), "%s %s", req->method, p);
        audit(opname, "-", "", "REJECT(unauthorized)", GetTickCount() - t0);
        respond_err(conn, 401, "unauthorized");
        return 1;
    }

    if (strcmp(req->method, "GET") == 0) {
        for (i = 0; i < 6; i++) {
            if (strcmp(p, GET_ROUTES[i]) != 0) continue;
            if (i == 0) get_health(conn);
            else if (i == 1) get_plugins(conn);
            else if (i == 2) get_manifest(conn);
            else if (i == 3) get_log(conn, req);
            else if (i == 4) get_pending(conn);
            else get_jobs(conn, req);
            return 1;
        }
        /* M4b: /api/v1/jobs/<id> (prefix; id charset is [a-z0-9-]) */
        if (strncmp(p, "/api/v1/jobs/", 13) == 0 && p[13]) {
            get_job_one(conn, p + 13);
            return 1;
        }
        _snprintf(opname, sizeof(opname), "GET %s", p);
        audit(opname, "-", "", "REJECT(unknown endpoint)", GetTickCount() - t0);
        respond_err(conn, 404, "unknown endpoint");
        return 1;
    }

    /* POST */
    for (i = 0; i < N_POST_ROUTES; i++)
        if (strcmp(p, POST_ROUTES[i].path) == 0) break;
    if (i >= N_POST_ROUTES) {
        _snprintf(opname, sizeof(opname), "POST %s", p);
        audit(opname, "-", "", "REJECT(unknown endpoint)", GetTickCount() - t0);
        respond_err(conn, 404, "unknown endpoint");
        return 1;
    }
    /* L4: JSON enforcement on every POST */
    if (strncmp(req->content_type, "application/json", 16) != 0) {
        audit(POST_ROUTES[i].op, "-", "", "REJECT(content-type)", GetTickCount() - t0);
        respond_err(conn, 415, "Content-Type must be application/json");
        return 1;
    }
    if (req->content_length < 0) {
        audit(POST_ROUTES[i].op, "-", "", "REJECT(length required)", GetTickCount() - t0);
        respond_err(conn, 411, "Content-Length required");
        return 1;
    }
    post_op(conn, req, POST_ROUTES[i].op, t0);
    return 1;
}
