/* creo-hub M4b: async job table (docs\39 3.2/3.3/3.4).
 *
 * Why a table: an `async:true` command answers 202 + jobId immediately, so the
 * httpd worker is free to serve GET /api/v1/jobs while the Creo main thread
 * keeps running the command. The table is written by the MAIN thread
 * (create/mark-working/finish from the executor) and read by httpd WORKER
 * threads -> one CRITICAL_SECTION.
 *
 * Progress: each job owns a named shared-memory slot (sdk\hub_jobslot.h) that
 * the plugin SDK writes (single writer, seq protocol) and readers map here.
 * Slot failure only degrades progress (pct -1), never the job. */
#ifndef JOBS_H
#define JOBS_H

#include <windows.h>
#include "cJSON.h"

#define HUB_JOB_MAX 32
#define HUB_JOB_TTL_MS 600000        /* finished jobs kept for 10 minutes */
#define HUB_JOB_RESULT_MAX 65536     /* same cap as RESULT_CAP */

/* state machine (names aligned with MCP Tasks SEP-2663, docs\39 4.6-3) */
#define HJ_FREE      0
#define HJ_QUEUED    1
#define HJ_WORKING   2
#define HJ_DONE      3
#define HJ_FAILED    4
#define HJ_CANCELLED 5

int  jobs_init(void);
void jobs_shutdown(void);

/* main thread: create a job before enqueue. Returns index (>=0) or -1 (full).
 * id_out gets "j-<n>"; slot_out gets the progress mapping name ("Local\...").
 * A NULL/empty slot_out name means progress is unavailable (job still valid). */
int  jobs_create(const char* plugin, const char* command, const char* risk,
                 int timeout_ms, char* id_out, int id_sz,
                 char* slot_out, int slot_sz);
void jobs_mark_working(int idx);
void jobs_cancel(int idx, const char* why);
/* ok=1 -> completed, 0 -> failed. result_json is copied (truncated at cap). */
void jobs_finish(int idx, int ok, const char* result_json, const char* state);

/* any thread: counts for /health */
int  jobs_count(int* running, int* queued);
int  jobs_find(const char* id);        /* index or -1 */

/* cJSON owned by caller. include_result=1 attaches the stored result text
 * (parsed as object; plain string if unparsable). */
cJSON* jobs_json_one(int idx, int include_result);
cJSON* jobs_json_list(int all);

const char* jobs_state_name(int state);

#endif /* JOBS_H */
