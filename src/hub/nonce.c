/* =====================================================================
 * creo-hub M3b b2: nonce gate implementation (contract in nonce.h;
 * frozen lifecycle docs\22 §6.3). Memory-only by design: hub restart
 * invalidates every slot (safe side - confirmation restarts with it).
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <bcrypt.h>

#include "nonce.h"
#include "ledger.h"

/* ------------------------------------------------------------ clock seam */

static unsigned long long nonce_now_default(void) { return GetTickCount64(); }
unsigned long long (*nonce_now_fn)(void) = nonce_now_default;

/* ------------------------------------------------------------ slot table */

typedef struct {
    NonceState state;
    char txn[32];
    char nonce[NONCE_HEX_LEN + 1];
    int  pidx;
    int  cidx;
    unsigned long long digest;
    char risk[12];
    unsigned long long issued_ms;
    unsigned long long approved_ms;
} NonceSlot;

static NonceSlot        g_slots[NONCE_SLOTS];
static CRITICAL_SECTION g_nonce_cs;
static int              g_nonce_inited = 0;
static unsigned long    g_txn_counter = 0;

void nonce_init(void)
{
    if (g_nonce_inited) return;
    InitializeCriticalSection(&g_nonce_cs);
    memset(g_slots, 0, sizeof(g_slots));
    g_nonce_inited = 1;
}

static int slot_expired(const NonceSlot* s, unsigned long long now)
{
    if (s->state == NONCE_FREE) return 0;
    /* M3e S1b: APPROVED slots age from approved_ms with the 10-min grace cap;
     * ISSUED/CONSUMED keep the 60s issued-based window. */
    if (s->state == NONCE_APPROVED)
        return (now - s->approved_ms) > NONCE_APPROVED_TTL_MS;
    return (now - s->issued_ms) > NONCE_TTL_MS;
}

/* ------------------------------------------------------------ digest */

typedef struct { char* b; size_t len, cap; int oom; } CB;

static void cb_need(CB* cb, size_t extra)
{
    size_t want;
    char* nb;
    if (cb->oom) return;
    if (cb->len + extra + 1 <= cb->cap) return;
    want = cb->cap ? cb->cap * 2 : 256;
    while (want < cb->len + extra + 1) want *= 2;
    nb = (char*)realloc(cb->b, want);
    if (!nb) { cb->oom = 1; return; }
    cb->b = nb;
    cb->cap = want;
}

static void cb_add(CB* cb, const char* s, size_t n)
{
    cb_need(cb, n);
    if (cb->oom) return;
    memcpy(cb->b + cb->len, s, n);
    cb->len += n;
}

static void cb_ch(CB* cb, char c) { cb_add(cb, &c, 1); }

static void canon(const cJSON* j, CB* cb);

static void canon_key(const char* key, CB* cb)
{
    /* escaped exactly like a JSON string leaf (reuse cJSON printer) */
    cJSON* tk = cJSON_CreateString(key ? key : "");
    char* kt = tk ? cJSON_PrintUnformatted(tk) : NULL;
    cJSON_Delete(tk);
    if (kt) { cb_add(cb, kt, strlen(kt)); cJSON_free(kt); }
    else    { cb->oom = 1; }
}

static int key_cmp(const void* a, const void* b)
{
    const cJSON* ja = *(const cJSON* const*)a;
    const cJSON* jb = *(const cJSON* const*)b;
    return strcmp(ja->string ? ja->string : "", jb->string ? jb->string : "");
}

static void canon(const cJSON* j, CB* cb)
{
    if (cb->oom) return;
    if (!j) { cb_add(cb, "null", 4); return; }
    if (cJSON_IsObject(j)) {
        int n = cJSON_GetArraySize((cJSON*)j), i;
        const cJSON** kids = NULL;
        cb_ch(cb, '{');
        if (n > 0) {
            const cJSON* it;
            kids = (const cJSON**)malloc(sizeof(*kids) * (size_t)n);
            if (!kids) { cb->oom = 1; return; }
            i = 0;
            for (it = j->child; it && i < n; it = it->next) kids[i++] = it;
            n = i;
            qsort(kids, (size_t)n, sizeof(*kids), key_cmp);
            for (i = 0; i < n && !cb->oom; i++) {
                if (i) cb_ch(cb, ',');
                canon_key(kids[i]->string, cb);
                cb_ch(cb, ':');
                canon(kids[i], cb);
            }
            free(kids);
        }
        cb_ch(cb, '}');
        return;
    }
    if (cJSON_IsArray(j)) {
        const cJSON* it;
        int first = 1;
        cb_ch(cb, '[');
        for (it = j->child; it && !cb->oom; it = it->next) {
            if (!first) cb_ch(cb, ',');
            first = 0;
            canon(it, cb);
        }
        cb_ch(cb, ']');
        return;
    }
    {
        /* scalars: reuse the cJSON leaf printer (deterministic per value) */
        char* t = cJSON_PrintUnformatted((cJSON*)j);
        if (t) { cb_add(cb, t, strlen(t)); cJSON_free(t); }
        else   { cb->oom = 1; }
    }
}

unsigned long long nonce_args_digest(const cJSON* args)
{
    /* FNV-1a 64 over the canonical serialization; NULL args = "{}"
     * (empty-object semantics, mirrors schema_val). */
    static const unsigned long long FNV_OFF = 1469598103934665603ULL;
    static const unsigned long long FNV_PRIME = 1099511628211ULL;
    CB cb;
    unsigned long long h = FNV_OFF;
    size_t i;

    memset(&cb, 0, sizeof(cb));
    if (args && cJSON_IsObject(args)) canon(args, &cb);
    else if (args) canon(args, &cb);          /* non-object args: digest what we got */
    else cb_add(&cb, "{}", 2);
    if (cb.oom) { free(cb.b); return 0; }     /* 0 = digest failed (caller fails closed) */
    for (i = 0; i < cb.len; i++) {
        h ^= (unsigned char)cb.b[i];
        h *= FNV_PRIME;
    }
    free(cb.b);
    return h;
}

void nonce_digest_hex(unsigned long long d, char out[17])
{
    _snprintf(out, 17, "%016llx", d);
    out[16] = '\0';
}

/* ------------------------------------------------------------ randomness */

static int fill_random_hex(char out[NONCE_HEX_LEN + 1])
{
    unsigned char raw[16];
    int i;
    NTSTATUS st = BCryptGenRandom(NULL, raw, sizeof(raw),
                                  BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (st != 0 /* STATUS_SUCCESS */) {
        /* fail-closed: no CSPRNG, no nonce (hub stays up; calls needing
         * confirmation get NR_TABLE_FULL -> 503 retry; practically
         * unreachable on Vista+ where bcrypt.dll is stock). */
        return 0;
    }
    for (i = 0; i < 16; i++)
        _snprintf(out + i * 2, 3, "%02x", raw[i]);
    out[NONCE_HEX_LEN] = '\0';
    return 1;
}

static void make_txn(char* buf, int bufsz)
{
    DWORD tick = GetTickCount();
    unsigned long mix = tick ^ (++g_txn_counter * 2654435761UL)
                        ^ (GetCurrentProcessId() << 3);
    _snprintf(buf, bufsz, "%08lx%08lx", (unsigned long)tick, mix);
    buf[bufsz - 1] = '\0';
}

/* ------------------------------------------------------------ slot ops */

/* caller holds CS. Pick a reclaimable slot index or -1:
 * FREE -> expired (any state) -> oldest ISSUED -> oldest CONSUMED.
 * A live APPROVED slot is NEVER stolen (a pending human decision wins). */
static int pick_slot_locked(unsigned long long now)
{
    int i, victim = -1, vstate = 99;
    unsigned long long vold = 0;
    for (i = 0; i < NONCE_SLOTS; i++) {
        NonceSlot* s = &g_slots[i];
        int cand_state = -1;
        if (s->state == NONCE_FREE) return i;                 /* first free wins */
        if (slot_expired(s, now)) cand_state = 0;             /* expired: any state */
        else if (s->state == NONCE_ISSUED) cand_state = 1;
        else if (s->state == NONCE_CONSUMED) cand_state = 2;
        if (cand_state >= 0) {
            if (cand_state < vstate ||
                (cand_state == vstate && s->issued_ms < vold)) {
                vstate = cand_state; victim = i; vold = s->issued_ms;
            }
            if (vstate == 0 && slot_expired(s, now)) {
                /* keep scanning for the OLDEST expired */
            }
        }
    }
    return victim;
}

static void fill_ticket_locked(const NonceSlot* s, NonceTicket* t)
{
    if (!t) return;
    strncpy(t->txn, s->txn, sizeof(t->txn) - 1);      t->txn[sizeof(t->txn) - 1] = '\0';
    strncpy(t->nonce, s->nonce, sizeof(t->nonce) - 1); t->nonce[sizeof(t->nonce) - 1] = '\0';
    strncpy(t->nonce_id, s->nonce, 8);                 t->nonce_id[8] = '\0';
    t->digest = s->digest;
}

NonceResult nonce_issue(int pidx, int cidx, unsigned long long digest,
                        const char* risk, NonceTicket* t)
{
    unsigned long long now;
    int i;
    NonceSlot* s;
    NonceResult res = NR_OK;

    nonce_init();
    now = nonce_now_fn();
    EnterCriticalSection(&g_nonce_cs);
    i = pick_slot_locked(now);
    if (i < 0) { LeaveCriticalSection(&g_nonce_cs); return NR_TABLE_FULL; }
    s = &g_slots[i];
    if (s->state != NONCE_FREE)
        ledger_release(s->pidx, "nonce", i);      /* reclaimed slot: old ownership ends */
    if (!fill_random_hex(s->nonce)) {
        s->state = NONCE_FREE;
        LeaveCriticalSection(&g_nonce_cs);
        return NR_TABLE_FULL;   /* fail-closed on CSPRNG failure */
    }
    make_txn(s->txn, sizeof(s->txn));
    s->pidx = pidx;
    s->cidx = cidx;
    s->digest = digest;
    strncpy(s->risk, risk ? risk : "confirm", sizeof(s->risk) - 1);
    s->risk[sizeof(s->risk) - 1] = '\0';
    s->issued_ms = now;
    s->approved_ms = 0;
    s->state = NONCE_ISSUED;
    if (!ledger_add(pidx, "nonce", i, NULL, NULL)) {
        /* P2 ledger full = hub bookkeeping broken: fail closed */
        s->state = NONCE_FREE;
        LeaveCriticalSection(&g_nonce_cs);
        return NR_TABLE_FULL;
    }
    fill_ticket_locked(s, t);
    LeaveCriticalSection(&g_nonce_cs);
    return res;
}

static NonceResult classify_locked(const NonceSlot* s, int pidx, int cidx,
                                   unsigned long long digest, unsigned long long now)
{
    if (slot_expired(s, now)) return NR_EXPIRED;
    switch (s->state) {
    case NONCE_ISSUED:   return NR_PENDING_APPROVAL;
    case NONCE_CONSUMED: return NR_CONSUMED;
    case NONCE_APPROVED:
        if (s->pidx != pidx || s->cidx != cidx || s->digest != digest)
            return NR_ARGS_MISMATCH;
        return NR_OK;
    default:             return NR_NOT_FOUND;
    }
}

NonceResult nonce_verify(const char* nonce, int pidx, int cidx,
                         unsigned long long digest)
{
    int i;
    NonceResult res = NR_NOT_FOUND;
    unsigned long long now;
    if (!nonce || !nonce[0]) return NR_NOT_FOUND;
    nonce_init();
    now = nonce_now_fn();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS; i++) {
        if (g_slots[i].state != NONCE_FREE &&
            strcmp(g_slots[i].nonce, nonce) == 0) {
            res = classify_locked(&g_slots[i], pidx, cidx, digest, now);
            break;
        }
    }
    LeaveCriticalSection(&g_nonce_cs);
    return res;
}

NonceResult nonce_consume(int pidx, int cidx, unsigned long long digest,
                          const char* nonce_or_null, NonceTicket* t)
{
    int i;
    NonceResult res = NR_NOT_FOUND;
    unsigned long long now;

    nonce_init();
    now = nonce_now_fn();
    EnterCriticalSection(&g_nonce_cs);
    if (nonce_or_null && nonce_or_null[0]) {
        for (i = 0; i < NONCE_SLOTS; i++) {
            if (g_slots[i].state != NONCE_FREE &&
                strcmp(g_slots[i].nonce, nonce_or_null) == 0) {
                res = classify_locked(&g_slots[i], pidx, cidx, digest, now);
                if (res == NR_OK) {
                    g_slots[i].state = NONCE_CONSUMED;
                    ledger_release(pidx, "nonce", i);
                    fill_ticket_locked(&g_slots[i], t);
                }
                break;
            }
        }
    } else {
        /* auto-pair: live APPROVED slot, same triple, within the APPROVED
         * grace window (M3e S1b: slot_expired ages APPROVED from approved_ms
         * with NONCE_APPROVED_TTL_MS; L3 panel flow, nonce stays in loop). */
        for (i = 0; i < NONCE_SLOTS; i++) {
            NonceSlot* s = &g_slots[i];
            if (s->state == NONCE_APPROVED && !slot_expired(s, now) &&
                s->pidx == pidx && s->cidx == cidx && s->digest == digest) {
                s->state = NONCE_CONSUMED;
                ledger_release(pidx, "nonce", i);
                fill_ticket_locked(s, t);
                res = NR_OK;
                break;
            }
        }
        if (res != NR_OK) res = NR_EXPIRED;   /* approval window closed */
    }
    LeaveCriticalSection(&g_nonce_cs);
    return res;
}

NonceResult nonce_approve_txn(const char* txn)
{
    int i;
    NonceResult res = NR_NOT_FOUND;
    unsigned long long now;
    if (!txn || !txn[0]) return NR_NOT_FOUND;
    nonce_init();
    now = nonce_now_fn();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS; i++) {
        if (g_slots[i].state != NONCE_FREE &&
            strcmp(g_slots[i].txn, txn) == 0) {
            if (slot_expired(&g_slots[i], now)) res = NR_EXPIRED;
            else if (g_slots[i].state == NONCE_CONSUMED) res = NR_CONSUMED;
            else {
                g_slots[i].state = NONCE_APPROVED;
                g_slots[i].approved_ms = now;
                res = NR_OK;
            }
            break;
        }
    }
    LeaveCriticalSection(&g_nonce_cs);
    return res;
}

NonceResult nonce_deny_txn(const char* txn)
{
    int i;
    NonceResult res = NR_NOT_FOUND;
    if (!txn || !txn[0]) return NR_NOT_FOUND;
    nonce_init();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS; i++) {
        if (g_slots[i].state != NONCE_FREE &&
            strcmp(g_slots[i].txn, txn) == 0) {
            if (g_slots[i].state == NONCE_CONSUMED) res = NR_CONSUMED;
            else {
                ledger_release(g_slots[i].pidx, "nonce", i);
                memset(&g_slots[i], 0, sizeof(g_slots[i]));   /* scrub nonce */
                g_slots[i].state = NONCE_FREE;
                res = NR_OK;
            }
            break;
        }
    }
    LeaveCriticalSection(&g_nonce_cs);
    return res;
}

int nonce_auto_pair_peek(int pidx, int cidx, unsigned long long digest)
{
    int i, found = 0;
    unsigned long long now;
    nonce_init();
    now = nonce_now_fn();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS; i++) {
        NonceSlot* s = &g_slots[i];
        /* M3e S1b audit fix: peek ages APPROVED via slot_expired (10-min grace
         * from approved_ms); the old extra <=60s-from-approval cap here left
         * the BARE-retry AUTOPAIR path (the live L3 flow) uncovered. */
        if (s->state == NONCE_APPROVED && !slot_expired(s, now) &&
            s->pidx == pidx && s->cidx == cidx && s->digest == digest) {
            found = 1;
            break;
        }
    }
    LeaveCriticalSection(&g_nonce_cs);
    return found;
}

void nonce_invalidate_plugin(int pidx)
{
    int i;
    nonce_init();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS; i++) {
        if (g_slots[i].state != NONCE_FREE && g_slots[i].pidx == pidx) {
            ledger_release(pidx, "nonce", i);
            memset(&g_slots[i], 0, sizeof(g_slots[i]));   /* scrub nonce */
            g_slots[i].state = NONCE_FREE;
        }
    }
    LeaveCriticalSection(&g_nonce_cs);
}

void nonce_invalidate_all(void)
{
    int i;
    nonce_init();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS; i++) {
        if (g_slots[i].state != NONCE_FREE)
            ledger_release(g_slots[i].pidx, "nonce", i);
        memset(&g_slots[i], 0, sizeof(g_slots[i]));
        g_slots[i].state = NONCE_FREE;
    }
    LeaveCriticalSection(&g_nonce_cs);
}

int nonce_live_count(void)
{
    int i, n = 0;
    nonce_init();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS; i++) if (g_slots[i].state != NONCE_FREE) n++;
    LeaveCriticalSection(&g_nonce_cs);
    return n;
}

int nonce_list_pending(NoncePendingInfo* out, int cap)
{
    unsigned long long now;
    int i, n = 0;
    if (!out || cap <= 0) return 0;
    nonce_init();
    now = nonce_now_fn();
    EnterCriticalSection(&g_nonce_cs);
    for (i = 0; i < NONCE_SLOTS && n < cap; i++) {
        NonceSlot* s = &g_slots[i];
        unsigned long long ref;
        if (s->state != NONCE_ISSUED && s->state != NONCE_APPROVED) continue;
        if (slot_expired(s, now)) continue;              /* dead ticket: not actionable */
        ref = (s->state == NONCE_APPROVED && s->approved_ms) ? s->approved_ms
                                                             : s->issued_ms;
        out[n].approved = (s->state == NONCE_APPROVED) ? 1 : 0;
        /* M3e S1b: per-state window (ISSUED 60s / APPROVED 10-min grace). */
        {
            unsigned long long win = (s->state == NONCE_APPROVED)
                                         ? NONCE_APPROVED_TTL_MS : NONCE_TTL_MS;
            out[n].remaining_ms = (now - ref < win) ? (win - (now - ref)) : 0;
        }
        strncpy(out[n].txn, s->txn, sizeof(out[n].txn) - 1);
        out[n].txn[sizeof(out[n].txn) - 1] = '\0';
        /* MASKED identity: first 8 hex only (nonce.h discipline) */
        strncpy(out[n].nonce_id, s->nonce, 8);
        out[n].nonce_id[8] = '\0';
        out[n].pidx = s->pidx;
        out[n].cidx = s->cidx;
        out[n].digest = s->digest;
        strncpy(out[n].risk, s->risk, sizeof(out[n].risk) - 1);
        out[n].risk[sizeof(out[n].risk) - 1] = '\0';
        n++;
    }
    LeaveCriticalSection(&g_nonce_cs);
    return n;
}
