/* creo-hub M3b b1: /call executor (docs\22 §6.1 frozen shapes; docs\23 §3.1).
 * Split-face design:
 *  - precheck runs on the httpd WORKER thread: full fast-fail validation chain
 *    (plugin -> command -> state==ACTIVE -> schema value-domain -> risk gate);
 *    any failure responds directly WITHOUT touching the envelope queue.
 *  - execute runs on the EXECUTOR thread (= Creo main thread) after dequeue:
 *    re-resolves target (TOCTOU guard), builds IN_JSON (+_ctx budget), calls
 *    through adapter route B (default) / A (registry-declarable), and shapes
 *    the 200/500 response with the host state token (启示 8).
 * b2 adds: nonce issuance/consumption, dryRun, orphan 504 protocol, txn audit. */
#ifndef CALL_EXEC_H
#define CALL_EXEC_H

#include "cJSON.h"
#include "registry.h"

typedef struct {
    char plugin[64];
    char command[64];
    int  timeout_ms;      /* command budget: timeoutSec*1000, default 10000 */
    int  cidx;            /* resolved command index (precheck -> diagnostics) */
    int  idempotent;      /* b2: drives the 504 retryable flag (frozen §6.1) */
    int  is_async;        /* M4b: async:true -> 202 + jobId (docs\39 3.1) */
    char risk[12];        /* M4b: for job display/audit */
} CallTarget;

/* Worker-thread validation chain. Returns 1 = pass (tgt filled; caller then
 * enqueues op "call" with the RAW body and tgt->timeout_ms). Returns 0 =
 * fast-fail: resp (respsz bytes) holds the frozen-shape JSON body and
 * *status the HTTP code (400/404/409/422/428). */
int call_exec_precheck(const cJSON* body, CallTarget* tgt,
                       char* resp, int respsz, int* status);

/* Executor-thread run. Same contract as execute_op cases: fills out (JSON
 * text), idout (plugin id for audit), stateout (audit result fragment);
 * returns HTTP status (200 success/business-fail; 500 infrastructure). */
int call_exec_execute(const char* payload, char* out, int outsz,
                      char* idout, char* stateout);

#endif /* CALL_EXEC_H */
