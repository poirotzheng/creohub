/* creo-hub M4b: async job table implementation (docs\39 3.2/3.3/3.4).
 * Single CRITICAL_SECTION; main thread writes, httpd worker threads read.
 * Progress slots: see sdk\hub_jobslot.h (single writer = plugin SDK,
 * seq odd/even protocol; readers re-read at most 3 times). */
#include "jobs.h"
#include "hub_jobslot.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef unsigned long long URLLIB_TICK;

typedef struct {
    int   used;
    char  id[16];
    char  plugin[64];
    char  command[96];
    char  risk[12];
    int   state;
    int   timeout_ms;
    char  last_state[48];        /* audit fragment at finish ("ok" / err_code) */
    URLLIB_TICK created_tick;
    URLLIB_TICK started_tick;
    URLLIB_TICK ended_tick;
    char* result;                /* heap (cap HUB_JOB_RESULT_MAX), freed on evict */
    char  slot_name[96];
    HANDLE mapping;
    HubJobSlot* view;
} HubJobRec;

static HubJobRec g_jobs[HUB_JOB_MAX];
static CRITICAL_SECTION g_jobs_cs;
static int g_jobs_inited = 0;
static unsigned long g_job_seq = 0;

static URLLIB_TICK now_tick(void) { return (URLLIB_TICK)GetTickCount64(); }

const char* jobs_state_name(int state)
{
    switch (state) {
    case HJ_QUEUED:    return "queued";
    case HJ_WORKING:   return "working";
    case HJ_DONE:      return "completed";
    case HJ_FAILED:    return "failed";
    case HJ_CANCELLED: return "cancelled";
    default:           return "unknown";
    }
}

int jobs_init(void)
{
    memset(g_jobs, 0, sizeof(g_jobs));
    InitializeCriticalSection(&g_jobs_cs);
    g_jobs_inited = 1;
    return 0;
}

static void job_free_slot(HubJobRec* j)
{
    if (j->view) { UnmapViewOfFile(j->view); j->view = NULL; }
    if (j->mapping) { CloseHandle(j->mapping); j->mapping = NULL; }
    if (j->result) { free(j->result); j->result = NULL; }
    j->used = 0;
    j->state = HJ_FREE;
    j->id[0] = '\0';
    j->slot_name[0] = '\0';
}

void jobs_shutdown(void)
{
    int i;
    if (!g_jobs_inited) return;
    EnterCriticalSection(&g_jobs_cs);
    for (i = 0; i < HUB_JOB_MAX; i++) job_free_slot(&g_jobs[i]);
    LeaveCriticalSection(&g_jobs_cs);
    DeleteCriticalSection(&g_jobs_cs);
    g_jobs_inited = 0;
}

/* caller holds the lock */
static void jobs_sweep_locked(void)
{
    int i;
    URLLIB_TICK t = now_tick();
    for (i = 0; i < HUB_JOB_MAX; i++) {
        HubJobRec* j = &g_jobs[i];
        if (!j->used) continue;
        if (j->state == HJ_DONE || j->state == HJ_FAILED || j->state == HJ_CANCELLED) {
            if (j->ended_tick && (t - j->ended_tick) > HUB_JOB_TTL_MS)
                job_free_slot(j);
        }
    }
}

int jobs_create(const char* plugin, const char* command, const char* risk,
                int timeout_ms, char* id_out, int id_sz,
                char* slot_out, int slot_sz)
{
    int i, slot = -1;
    HubJobRec* j;
    if (!g_jobs_inited) return -1;
    if (id_out && id_sz > 0) id_out[0] = '\0';
    if (slot_out && slot_sz > 0) slot_out[0] = '\0';
    EnterCriticalSection(&g_jobs_cs);
    jobs_sweep_locked();
    for (i = 0; i < HUB_JOB_MAX; i++) {
        if (!g_jobs[i].used) { slot = i; break; }
    }
    if (slot < 0) { LeaveCriticalSection(&g_jobs_cs); return -1; }
    j = &g_jobs[slot];
    memset(j, 0, sizeof(*j));
    j->used = 1;
    j->state = HJ_QUEUED;
    j->timeout_ms = timeout_ms;
    j->created_tick = now_tick();
    g_job_seq++;
    _snprintf(j->id, sizeof(j->id), "j-%lu", g_job_seq);
    strncpy(j->plugin, plugin ? plugin : "", sizeof(j->plugin) - 1);
    strncpy(j->command, command ? command : "", sizeof(j->command) - 1);
    strncpy(j->risk, risk ? risk : "safe", sizeof(j->risk) - 1);
    _snprintf(j->slot_name, sizeof(j->slot_name), "Local\\creo_hub_job_%lu_%lu",
              (unsigned long)GetCurrentProcessId(), g_job_seq);
    /* progress slot (best effort: failure = no progress, job still valid) */
    j->mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, 4096, j->slot_name);
    if (j->mapping) {
        HubJobSlot* v = (HubJobSlot*)MapViewOfFile(j->mapping, FILE_MAP_ALL_ACCESS,
                                                   0, 0, sizeof(HubJobSlot));
        if (v) {
            memset(v, 0, sizeof(*v));
            v->magic = HUB_JOBSLOT_MAGIC;
            v->state = HUB_JOBSLOT_ST_WORKING;
            v->pct = HUB_JOBSLOT_PCT_UNKNOWN;
            j->view = v;
        } else {
            CloseHandle(j->mapping);
            j->mapping = NULL;
            j->slot_name[0] = '\0';
        }
    } else {
        j->slot_name[0] = '\0';
    }
    if (id_out && id_sz > 0) { strncpy(id_out, j->id, id_sz - 1); id_out[id_sz - 1] = '\0'; }
    if (slot_out && slot_sz > 0) { strncpy(slot_out, j->slot_name, slot_sz - 1); slot_out[slot_sz - 1] = '\0'; }
    LeaveCriticalSection(&g_jobs_cs);
    return slot;
}

void jobs_mark_working(int idx)
{
    if (!g_jobs_inited || idx < 0 || idx >= HUB_JOB_MAX) return;
    EnterCriticalSection(&g_jobs_cs);
    if (g_jobs[idx].used) {
        g_jobs[idx].state = HJ_WORKING;
        g_jobs[idx].started_tick = now_tick();
    }
    LeaveCriticalSection(&g_jobs_cs);
}

void jobs_cancel(int idx, const char* why)
{
    if (!g_jobs_inited || idx < 0 || idx >= HUB_JOB_MAX) return;
    EnterCriticalSection(&g_jobs_cs);
    if (g_jobs[idx].used) {
        g_jobs[idx].state = HJ_CANCELLED;
        g_jobs[idx].ended_tick = now_tick();
        _snprintf(g_jobs[idx].last_state, sizeof(g_jobs[idx].last_state), "%s",
                  why ? why : "cancelled");
        if (g_jobs[idx].view) {
            HubJobSlot* v = g_jobs[idx].view;
            v->seq++; v->state = HUB_JOBSLOT_ST_CANCELLED; v->seq++;
        }
    }
    LeaveCriticalSection(&g_jobs_cs);
}

static void jobs_slot_write_final(HubJobSlot* v, int ok)
{
    if (!v) return;
    v->seq++;
    v->state = ok ? HUB_JOBSLOT_ST_DONE : HUB_JOBSLOT_ST_FAILED;
    v->pct = 100;
    v->seq++;
}

void jobs_finish(int idx, int ok, const char* result_json, const char* state)
{
    size_t n;
    if (!g_jobs_inited || idx < 0 || idx >= HUB_JOB_MAX) return;
    EnterCriticalSection(&g_jobs_cs);
    if (g_jobs[idx].used) {
        HubJobRec* j = &g_jobs[idx];
        j->state = ok ? HJ_DONE : HJ_FAILED;
        j->ended_tick = now_tick();
        _snprintf(j->last_state, sizeof(j->last_state), "%s", state ? state : (ok ? "ok" : "failed"));
        if (j->result) { free(j->result); j->result = NULL; }
        if (result_json && result_json[0]) {
            n = strlen(result_json);
            if (n >= HUB_JOB_RESULT_MAX) n = HUB_JOB_RESULT_MAX - 1;
            j->result = (char*)malloc(n + 1);
            if (j->result) { memcpy(j->result, result_json, n); j->result[n] = '\0'; }
        }
        jobs_slot_write_final(j->view, ok);
    }
    LeaveCriticalSection(&g_jobs_cs);
}

int jobs_count(int* running, int* queued)
{
    int i, run = 0, q = 0;
    if (running) *running = 0;
    if (queued) *queued = 0;
    if (!g_jobs_inited) return 0;
    EnterCriticalSection(&g_jobs_cs);
    for (i = 0; i < HUB_JOB_MAX; i++) {
        if (!g_jobs[i].used) continue;
        if (g_jobs[i].state == HJ_WORKING) run++;
        else if (g_jobs[i].state == HJ_QUEUED) q++;
    }
    LeaveCriticalSection(&g_jobs_cs);
    if (running) *running = run;
    if (queued) *queued = q;
    return run + q;
}

int jobs_find(const char* id)
{
    int i, found = -1;
    if (!g_jobs_inited || !id) return -1;
    EnterCriticalSection(&g_jobs_cs);
    for (i = 0; i < HUB_JOB_MAX; i++) {
        if (g_jobs[i].used && strcmp(g_jobs[i].id, id) == 0) { found = i; break; }
    }
    LeaveCriticalSection(&g_jobs_cs);
    return found;
}

/* read progress from the shared slot (torn-read safe: seq odd/even) */
static void jobs_read_slot(HubJobRec* j, int* pct, char* msg, int msgsz, int* slot_state)
{
    int tries;
    *pct = HUB_JOBSLOT_PCT_UNKNOWN;
    if (msgsz > 0) msg[0] = '\0';
    *slot_state = HUB_JOBSLOT_ST_NONE;
    if (!j->view) return;
    for (tries = 0; tries < 3; tries++) {
        unsigned long s1 = j->view->seq;
        int st, p;
        char m[sizeof(j->view->msg)];
        if (s1 & 1UL) continue;
        st = j->view->state;
        p = j->view->pct;
        memcpy(m, (const void*)j->view->msg, sizeof(m));
        if (j->view->seq != s1) continue;
        *pct = p;
        *slot_state = st;
        if (msgsz > 0) {
            m[sizeof(m) - 1] = '\0';
            strncpy(msg, m, msgsz - 1);
            msg[msgsz - 1] = '\0';
        }
        return;
    }
}

cJSON* jobs_json_one(int idx, int include_result)
{
    cJSON* o = NULL;
    if (!g_jobs_inited || idx < 0 || idx >= HUB_JOB_MAX) return NULL;
    EnterCriticalSection(&g_jobs_cs);
    if (g_jobs[idx].used) {
        HubJobRec* j = &g_jobs[idx];
        int pct, slot_state;
        char msg[200];
        URLLIB_TICK endt;
        jobs_read_slot(j, &pct, msg, sizeof(msg), &slot_state);
        endt = j->ended_tick ? j->ended_tick : now_tick();
        o = cJSON_CreateObject();
        if (o) {
            cJSON_AddStringToObject(o, "jobId", j->id);
            cJSON_AddStringToObject(o, "plugin", j->plugin);
            cJSON_AddStringToObject(o, "command", j->command);
            cJSON_AddStringToObject(o, "risk", j->risk);
            cJSON_AddStringToObject(o, "state", jobs_state_name(j->state));
            cJSON_AddNumberToObject(o, "pct", pct);
            if (msg[0]) cJSON_AddStringToObject(o, "msg", msg);
            cJSON_AddNumberToObject(o, "elapsedMs",
                                    (double)(endt - (j->started_tick ? j->started_tick : j->created_tick)));
            cJSON_AddNumberToObject(o, "timeoutMs", j->timeout_ms);
            cJSON_AddStringToObject(o, "auditState", j->last_state);
            if (include_result && j->result) {
                cJSON* parsed = cJSON_Parse(j->result);
                if (parsed) cJSON_AddItemToObject(o, "result", parsed);
                else cJSON_AddStringToObject(o, "result", j->result);
            }
        }
    }
    LeaveCriticalSection(&g_jobs_cs);
    return o;
}

cJSON* jobs_json_list(int all)
{
    cJSON* arr;
    int i;
    if (!g_jobs_inited) return NULL;
    arr = cJSON_CreateArray();
    if (!arr) return NULL;
    EnterCriticalSection(&g_jobs_cs);
    for (i = 0; i < HUB_JOB_MAX; i++) {
        if (!g_jobs[i].used) continue;
        if (!all && g_jobs[i].state != HJ_QUEUED && g_jobs[i].state != HJ_WORKING) continue;
        {
            HubJobRec* j = &g_jobs[i];
            int pct, slot_state;
            char msg[200];
            cJSON* o;
            jobs_read_slot(j, &pct, msg, sizeof(msg), &slot_state);
            o = cJSON_CreateObject();
            if (!o) continue;
            cJSON_AddStringToObject(o, "jobId", j->id);
            cJSON_AddStringToObject(o, "plugin", j->plugin);
            cJSON_AddStringToObject(o, "command", j->command);
            cJSON_AddStringToObject(o, "risk", j->risk);
            cJSON_AddStringToObject(o, "state", jobs_state_name(j->state));
            cJSON_AddNumberToObject(o, "pct", pct);
            if (msg[0]) cJSON_AddStringToObject(o, "msg", msg);
            cJSON_AddNumberToObject(o, "elapsedMs",
                                    (double)((j->ended_tick ? j->ended_tick : now_tick()) -
                                             (j->started_tick ? j->started_tick : j->created_tick)));
            cJSON_AddNumberToObject(o, "timeoutMs", j->timeout_ms);
            cJSON_AddItemToArray(arr, o);
        }
    }
    LeaveCriticalSection(&g_jobs_cs);
    return arr;
}
