/* creo-hub M2: web_api - HTTP API face (docs\09 4 contract, docs\13 G1/G2/G5,
 * docs\15 R3). Two-thread split:
 *   - httpd worker thread: token/JSON gate (L3/L4), read-only GET routes
 *     answered directly, mutating POSTs submitted as G1 envelopes.
 *   - executor thread (Creo main thread via E6 marshaling in M2b; a pump
 *     thread in offline tests): web_api_run_pending() executes ONE envelope
 *     against registry/reconcile, stamps seq, signals completion.
 * All registry mutation happens ONLY on the executor thread (G3 discipline:
 * external input edits target, committed converges through guarded ops).
 * GET routes read g_plugins lock-free for display; torn/stale reads are
 * benign (next 2s poll corrects) - documented M2 tradeoff. */
#ifndef WEB_API_H
#define WEB_API_H

#include "httpd.h"

/* Lifecycle: init before httpd_start; shutdown after httpd_stop.
 * Terminate is TWO-PHASE: set_executor_ready(0) + stop_waiters() BEFORE
 * httpd_stop() (wakes in-flight POST waiters immediately, so the worker
 * join cannot stall for a full op timeout), then shutdown() AFTER the join
 * (only then are event handles closed / CS deleted - no live waiter). */
int  web_api_init(void);
void web_api_shutdown(void);
void web_api_stop_waiters(void);

/* L3: per-start secret (hub_main generates 128-bit random hex in M2b).
 * Empty token => every API call is rejected 401. Never logged. */
void web_api_set_token(const char* hex_token);

/* Executor readiness: 0 => POSTs get 503 "hub not ready" (no queueing into
 * a void). hub_main sets 1 once the E6 marshaling pump exists. */
void web_api_set_executor_ready(int ready);

/* E6 wakeup: called on the httpd worker thread right after an envelope is
 * queued; hub_main registers a hub_marsh_post wrapper so the Creo main
 * thread pumps immediately (the 500ms ProUITimer stays as safety net).
 * NULL (default, offline tests) => executor polling drains the queue. */
void web_api_set_wakeup(void (*fn)(void));

/* httpd route hook for /api/v1/* - always returns 1 (handled). */
int  web_api_route(const HttpdRequest* req, HttpdConn* conn);

/* Executor side: run (or drop, if the caller abandoned) at most one pending
 * envelope. Returns 1 if work was done, 0 if the queue is empty. */
int  web_api_run_pending(void);

/* Test/ops introspection: number of occupied queue slots. */
int  web_api_queue_used(void);

#endif /* WEB_API_H */
