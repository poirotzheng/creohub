/* =====================================================================
 * creo-hub M3b b2: nonce gate (docs\22 §6.3 / docs\23 §3.3 / docs\25 §2 b2)
 *
 * 16 static slots {transactionId, nonce(128-bit BCryptGenRandom hex),
 * plugin_idx, cmd_idx, argsDigest(FNV-1a64 over canonical serialization),
 * risk, issued_ms, approved_ms, state}. Full -> evict oldest expired, then
 * oldest ISSUED, then oldest CONSUMED (never a live APPROVED). Hub restart
 * = all invalid (memory-only by design). Full nonce value NEVER logged -
 * audit carries nonce_id = first 8 hex only (M2.5 token discipline).
 *
 * Lifecycle (frozen, docs\22 §6.3):
 *   first call risk>=confirm without nonce -> issue (ISSUED) -> 428
 *   human approves (panel dialog / bridge-proxied elicitation) -> APPROVED
 *   retry WITH nonce: verify exists ^ in-TTL ^ APPROVED ^ (plugin,command,
 *     argsDigest) triple match -> enqueue; CONSUMED happens AT EXECUTE time
 *     (启示 11: never burn a nonce at enqueue for an execution that fails)
 *   retry WITHOUT nonce within the APPROVED grace window (M3e S1b: 10 min from
 *     approval; M3b original: 60s): auto-pair same triple
 *     (L3 panel flow - the nonce never leaves the hub/bridge loop)
 *   conflicts: digest mismatch -> NONCE_ARGS_MISMATCH; TTL over ->
 *     NONCE_EXPIRED; consumed reuse at precheck -> fresh 428 (docs\25);
 *     ISSUED-but-unapproved retry -> NONCE_PENDING_APPROVAL (RFC 8628
 *     authorization_pending analog, documented 4th subcode)
 *   plugin state change (unmount/ERROR/TAINTED/fingerprint) -> invalidate
 *     all of that plugin's slots (K8s webhook-bound-token isomorph)
 *
 * Thread safety: precheck/confirm run on httpd worker threads, consumption
 * on the pump (main) thread -> module owns a CRITICAL_SECTION.
 * Win7: bcrypt.dll is Vista+ stock (D-W7 whitelist). */
#ifndef NONCE_GATE_H
#define NONCE_GATE_H

#include "cJSON.h"

#define NONCE_SLOTS      16
#define NONCE_TTL_MS     60000ULL
/* M3e S1b (user-ratified 2026-10-12): two-stage lifetime. ISSUED keeps the 60s
 * attention window (unattended-ticket hygiene); APPROVED ages from approved_ms
 * with this 10-min grace cap (fixes the agent-retry-turn latency race: human
 * approval is fast, the retry turn is slow - six-ticket chronicle docs\29 S7).
 * Release rules (consume/deny/plugin-invalidation/restart), argsDigest binding
 * and single-use semantics unchanged. */
#define NONCE_APPROVED_TTL_MS 600000ULL
#define NONCE_HEX_LEN    32          /* 128-bit CSPRNG -> 32 hex chars */

typedef enum {
    NONCE_FREE = 0,
    NONCE_ISSUED,
    NONCE_APPROVED,
    NONCE_CONSUMED
} NonceState;

typedef enum {
    NR_OK = 0,
    NR_PENDING_APPROVAL,   /* slot ISSUED, human approval not registered */
    NR_ARGS_MISMATCH,      /* approved args != submitted args (digest) */
    NR_EXPIRED,            /* TTL over (or unknown nonce -> same client fix) */
    NR_CONSUMED,           /* one-shot violated: replay of a burned nonce */
    NR_NOT_FOUND,          /* no slot for this nonce/txn (evicted or bogus) */
    NR_TABLE_FULL          /* all 16 slots live-APPROVED (absurd; fail safe) */
} NonceResult;

/* Ticket = everything a caller may put on the wire / in an audit line.
 * nonce_id is the first 8 hex chars of nonce (audit-safe). */
typedef struct {
    char txn[32];
    char nonce[NONCE_HEX_LEN + 1];
    char nonce_id[12];
    unsigned long long digest;
} NonceTicket;

void nonce_init(void);   /* idempotent; call from web_api_init and tests */

/* Canonical args serialization (sorted keys, no whitespace, UTF-8 as-is)
 * then FNV-1a64. args==NULL digests as "{}" (empty-object semantics,
 * matches schema_val). Same rule must be used by any bridge-side
 * pre-check (docs\23 §3.3: documented digest algorithm). */
unsigned long long nonce_args_digest(const cJSON* args);
void nonce_digest_hex(unsigned long long d, char out[17]);

/* Issue a fresh ISSUED slot; fills ticket. NR_TABLE_FULL when no slot can
 * be reclaimed (caller -> 503 retry-after). */
NonceResult nonce_issue(int pidx, int cidx, unsigned long long digest,
                        const char* risk, NonceTicket* t);

/* Lookup by nonce; classify without consuming. */
NonceResult nonce_verify(const char* nonce, int pidx, int cidx,
                         unsigned long long digest);

/* Execute-time atomic consumption (single pump thread, but CS anyway):
 * nonce_or_null != NULL -> consume that slot (full verify first);
 * NULL -> auto-pair a live APPROVED slot with matching triple.
 * On NR_OK fills ticket (txn/nonce_id for the audit line). */
NonceResult nonce_consume(int pidx, int cidx, unsigned long long digest,
                          const char* nonce_or_null, NonceTicket* t);

/* Panel/bridge approval registration by transactionId. */
NonceResult nonce_approve_txn(const char* txn);
NonceResult nonce_deny_txn(const char* txn);      /* frees the slot */

/* True when a live APPROVED slot matches the triple (dryRun reporting). */
int nonce_auto_pair_peek(int pidx, int cidx, unsigned long long digest);

void nonce_invalidate_plugin(int pidx);
void nonce_invalidate_all(void);
int  nonce_live_count(void);

/* b4 panel feed (docs\25 b4 / docs\23 §4.3 confirm dialog): live tickets.
 * The full nonce stays MASKED (nonce_id = first 8 hex, audit discipline);
 * transactionId was already disclosed to the caller in the 428 response -
 * /pending is the token-gated loopback face of the DESIGNATED approver
 * (confirmVia "panel-required"), not a new disclosure channel. */
typedef struct {
    char txn[32];
    char nonce_id[12];
    int  pidx;
    int  cidx;
    unsigned long long digest;
    char risk[12];
    int  approved;                  /* 1 = APPROVED (awaiting execute),
                                     * 0 = ISSUED (awaiting human approval) */
    unsigned long long remaining_ms;
} NoncePendingInfo;

/* Fill up to cap entries with live (unexpired ISSUED/APPROVED) slots in
 * table order; returns the number written. */
int nonce_list_pending(NoncePendingInfo* out, int cap);

/* Test seam: wall clock source (default GetTickCount64). */
extern unsigned long long (*nonce_now_fn)(void);

#endif /* NONCE_GATE_H */
