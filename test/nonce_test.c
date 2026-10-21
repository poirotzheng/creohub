/* =====================================================================
 * creo-hub M3b b2 OFFLINE nonce_test: nonce gate unit tests (docs\25 §2 b2
 * gate D2: lifecycle / argsDigest binding / expiry / replay / consumed
 * race / invalidation / 16-slot eviction / auto-pair). No Creo, no PTK.
 * Exit: 0 = ALL PASS, 1 = failures.
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nonce.h"
#include "ledger.h"
#include "registry.h"
#include "cJSON.h"

static int g_pass = 0, g_fail = 0;
static void CHECK(int cond, const char* what)
{
    if (cond) { g_pass++; printf("  [PASS] %s\n", what); }
    else      { g_fail++; printf("  [FAIL] %s\n", what); }
}

/* ---- time seam: fake monotonic clock ---- */
static unsigned long long g_fake_now = 1000000ULL;
static unsigned long long fake_now_fn(void) { return g_fake_now; }

static unsigned long long digest_of(const char* json)
{
    cJSON* j = json ? cJSON_Parse(json) : NULL;
    unsigned long long d = nonce_args_digest(j);
    cJSON_Delete(j);
    return d;
}

int main(void)
{
    NonceTicket t1, t2, t3, t4, t5, tf;
    unsigned long long d1, d2, d1x, dnum, dnum2, dempty;
    int i, issued;

    printf("== creo-hub M3b b2 nonce_test (offline unit) ==\n");
    /* b3 P2: nonce slots carry ledger ownership keyed by plugin id - give the
     * unit test minimal registry records so ledger_add accepts the pidx. */
    g_n_plugins = 8;
    for (i = 0; i < g_n_plugins; i++) {
        memset(&g_plugins[i], 0, sizeof(g_plugins[i]));
        _snprintf(g_plugins[i].id, sizeof(g_plugins[i].id), "t-p%d", i);
    }
    nonce_init();
    nonce_now_fn = fake_now_fn;

    printf("--- argsDigest canonicalization ---\n");
    d1 = digest_of("{\"b\":1,\"a\":\"x\",\"o\":{\"z\":true,\"y\":[1,2,{\"q\":null}]}}");
    d2 = digest_of("{\"a\":\"x\",\"o\":{\"y\":[1,2,{\"q\":null}],\"z\":true},\"b\":1}");
    d1x = digest_of("{\"other\":1}");   /* genuinely different content */
    CHECK(d1 != 0 && d1 == d2, "key order + nesting: canonical digests equal");
    CHECK(d1 != digest_of("{\"a\":\"x\",\"b\":2,\"o\":{\"z\":true,\"y\":[1,2,{\"q\":null}]}}"),
          "value change -> different digest");
    CHECK(digest_of("{\"a\":\"x\"}") != digest_of("{\"a\":\"y\"}"),
          "string value binds into digest");
    dnum  = digest_of("{\"n\":5}");
    dnum2 = digest_of("{\"n\":5.0}");
    CHECK(dnum == dnum2, "numeric normalization 5 == 5.0");
    dempty = digest_of(NULL);
    CHECK(dempty == digest_of("{}"), "NULL args digest == empty object digest");
    CHECK(dempty != d1, "empty digest distinct from populated");
    {
        char hex[17];
        nonce_digest_hex(d1, hex);
        CHECK(strlen(hex) == 16, "digest hex = 16 chars");
    }

    printf("--- lifecycle: issue -> pending -> approve -> verify -> consume ---\n");
    CHECK(nonce_issue(0, 0, d1, "confirm", &t1) == NR_OK, "issue slot");
    CHECK(strlen(t1.nonce) == 32 && strlen(t1.nonce_id) == 8, "nonce 32 hex / nonce_id 8");
    CHECK(strncmp(t1.nonce, t1.nonce_id, 8) == 0, "nonce_id = nonce prefix (audit-safe)");
    CHECK(nonce_verify(t1.nonce, 0, 0, d1) == NR_PENDING_APPROVAL,
          "ISSUED + retry = PENDING_APPROVAL (human decision outstanding)");
    CHECK(nonce_verify("deadbeefdeadbeefdeadbeefdeadbeef", 0, 0, d1) == NR_NOT_FOUND,
          "unknown nonce -> NOT_FOUND");
    CHECK(nonce_approve_txn(t1.txn) == NR_OK, "approve by txn");
    CHECK(nonce_approve_txn(t1.txn) == NR_OK, "approve idempotent");
    CHECK(nonce_verify(t1.nonce, 0, 0, d1) == NR_OK, "APPROVED + triple match -> OK");
    CHECK(nonce_verify(t1.nonce, 0, 0, d1x) == NR_ARGS_MISMATCH,
          "APPROVED + digest differs -> ARGS_MISMATCH (batch A run B blocked)");
    CHECK(nonce_verify(t1.nonce, 1, 0, d1) == NR_ARGS_MISMATCH,
          "APPROVED + other plugin -> ARGS_MISMATCH");
    CHECK(nonce_auto_pair_peek(0, 0, d1) == 1, "auto-pair peek sees APPROVED triple");
    CHECK(nonce_auto_pair_peek(0, 0, d1x) == 0, "auto-pair peek rejects other digest");
    CHECK(nonce_consume(0, 0, d1, NULL, &t2) == NR_OK && strcmp(t2.txn, t1.txn) == 0,
          "auto-pair consume (no nonce presented) burns the APPROVED slot");
    CHECK(nonce_verify(t1.nonce, 0, 0, d1) == NR_CONSUMED, "post-consume verify -> CONSUMED");
    CHECK(nonce_consume(0, 0, d1, t1.nonce, &t2) == NR_CONSUMED,
          "replay with consumed nonce -> CONSUMED (one-shot)");
    CHECK(nonce_consume(0, 0, d1, NULL, &t2) == NR_EXPIRED,
          "auto-pair after burn -> EXPIRED (window closed)");
    CHECK(nonce_auto_pair_peek(0, 0, d1) == 0, "auto-pair peek empty after burn");

    printf("--- nonce-present consume path ---\n");
    CHECK(nonce_issue(0, 1, d1x, "destructive", &t3) == NR_OK, "issue #2");
    CHECK(nonce_approve_txn(t3.txn) == NR_OK, "approve #2");
    CHECK(nonce_consume(0, 1, d1x, t3.nonce, &t2) == NR_OK, "consume with nonce -> OK");
    CHECK(strcmp(t2.nonce_id, t3.nonce_id) == 0, "ticket carries nonce_id for audit");

    printf("--- TTL expiry: ISSUED 60s window (S1b stage 1) ---\n");
    CHECK(nonce_issue(0, 2, d1, "confirm", &t4) == NR_OK, "issue #3 (left UNAPPROVED)");
    g_fake_now += NONCE_TTL_MS + 1000;
    CHECK(nonce_verify(t4.nonce, 0, 2, d1) == NR_EXPIRED, "TTL over -> EXPIRED");
    CHECK(nonce_approve_txn(t4.txn) == NR_EXPIRED, "approve after TTL -> EXPIRED");
    CHECK(nonce_consume(0, 2, d1, t4.nonce, &t2) == NR_EXPIRED, "consume after TTL -> EXPIRED");

    printf("--- deny + invalidation ---\n");
    g_fake_now += 1000;
    CHECK(nonce_issue(1, 0, d2, "confirm", &t5) == NR_OK, "issue #4 (plugin 1)");
    CHECK(nonce_deny_txn(t5.txn) == NR_OK, "deny frees slot");
    CHECK(nonce_verify(t5.nonce, 1, 0, d2) == NR_NOT_FOUND, "denied nonce gone");
    CHECK(nonce_deny_txn(t5.txn) == NR_NOT_FOUND, "deny twice -> NOT_FOUND");
    CHECK(nonce_issue(2, 0, d1, "confirm", &t5) == NR_OK, "issue #5 (plugin 2)");
    CHECK(nonce_approve_txn(t5.txn) == NR_OK, "approve #5");
    nonce_invalidate_plugin(2);
    CHECK(nonce_verify(t5.nonce, 2, 0, d1) == NR_NOT_FOUND,
          "plugin state change invalidates its slots (unmount/ERROR/fingerprint)");
    CHECK(nonce_approve_txn("00000000ffffffff") == NR_NOT_FOUND, "approve unknown txn -> NOT_FOUND");

    printf("--- 16-slot table: full + eviction ---\n");
    nonce_invalidate_all();
    CHECK(nonce_live_count() == 0, "invalidate_all clears the table");
    issued = 0;
    for (i = 0; i < NONCE_SLOTS; i++) {
        if (nonce_issue(5, i, d1 + (unsigned long long)i, "confirm", &tf) == NR_OK) {
            nonce_approve_txn(tf.txn);
            issued++;
        }
    }
    CHECK(issued == NONCE_SLOTS, "16 slots all issued+approved");
    CHECK(nonce_live_count() == NONCE_SLOTS, "live count = 16");
    CHECK(nonce_issue(5, 99, d1, "confirm", &tf) == NR_TABLE_FULL,
          "17th issue over live APPROVED slots -> TABLE_FULL (fail safe, caller 503)");
    g_fake_now += NONCE_TTL_MS + 1000;
    CHECK(nonce_issue(5, 99, d1, "confirm", &tf) == NR_TABLE_FULL,
          "S1b 61s post-issue: live APPROVED slots still block (grace window)");
    g_fake_now += NONCE_APPROVED_TTL_MS + 1000;
    CHECK(nonce_issue(5, 99, d1, "confirm", &tf) == NR_OK,
          "after grace cap the oldest expired slot is reclaimed");

    printf("--- P2 ledger ownership (b3) ---\n");
    nonce_invalidate_all();
    CHECK(ledger_count(3) == 0, "P2 ledger clean after invalidate_all");
    CHECK(nonce_issue(3, 0, d1, "confirm", &tf) == NR_OK, "issue for ledger check");
    CHECK(ledger_count(3) == 1, "P2 ledger: nonce slot owned by plugin 3");
    CHECK(nonce_approve_txn(tf.txn) == NR_OK, "approve for ledger check");
    CHECK(nonce_consume(3, 0, d1, NULL, &t1) == NR_OK, "auto-pair consume");
    CHECK(ledger_count(3) == 0, "P2 ledger: ownership released at consume");
    CHECK(nonce_issue(3, 1, d2, "confirm", &tf) == NR_OK, "issue #2 for drain check");
    CHECK(ledger_drain(3) == 1, "P2 ledger drain releases residual (unmount order)");
    CHECK(ledger_count(3) == 0 &&
          nonce_verify(tf.nonce, 3, 1, d2) == NR_PENDING_APPROVAL,
          "drain is bookkeeping only (slot state untouched)");
    nonce_invalidate_all();

    printf("--- b4 nonce_list_pending (panel confirm-dialog feed) ---\n");
    {
        NoncePendingInfo infos[NONCE_SLOTS];
        int n;
        g_fake_now += NONCE_TTL_MS + 1000;   /* fresh TTL window */
        CHECK(nonce_list_pending(infos, NONCE_SLOTS) == 0, "b4 list empty on clean table");
        CHECK(nonce_issue(4, 0, d1, "destructive", &tf) == NR_OK, "b4 issue for list");
        n = nonce_list_pending(infos, NONCE_SLOTS);
        CHECK(n == 1, "b4 list shows the live ISSUED ticket");
        CHECK(infos[0].pidx == 4 && infos[0].cidx == 0 && infos[0].digest == d1,
              "b4 list carries the binding triple (pidx/cidx/digest)");
        CHECK(strcmp(infos[0].risk, "destructive") == 0 && infos[0].approved == 0,
              "b4 list carries risk + state=issued");
        CHECK(strlen(infos[0].nonce_id) == 8 && strncmp(infos[0].nonce_id, tf.nonce, 8) == 0,
              "b4 nonce MASKED: nonce_id = first 8 hex only");
        CHECK(strcmp(infos[0].txn, tf.txn) == 0, "b4 txn = the approve handle");
        CHECK(infos[0].remaining_ms > 0 && infos[0].remaining_ms <= NONCE_TTL_MS,
              "b4 remaining_ms inside TTL (countdown feed)");
        CHECK(nonce_approve_txn(tf.txn) == NR_OK, "b4 approve for list state flip");
        n = nonce_list_pending(infos, NONCE_SLOTS);
        CHECK(n == 1 && infos[0].approved == 1, "b4 approved ticket relisted state=approved");
        /* M3e S1b: approved ticket survives the 60s issued-window (grace cap
         * 10 min from approval) and its countdown feed switches window. */
        CHECK(infos[0].remaining_ms > NONCE_TTL_MS &&
              infos[0].remaining_ms <= NONCE_APPROVED_TTL_MS,
              "S1b approved remaining_ms inside 10-min grace window");
        g_fake_now += NONCE_TTL_MS + 1000;
        n = nonce_list_pending(infos, NONCE_SLOTS);
        CHECK(n == 1 && infos[0].approved == 1,
              "S1b approved ticket still listed 61s after approval (race fix)");
        g_fake_now += NONCE_APPROVED_TTL_MS + 1000;
        CHECK(nonce_list_pending(infos, NONCE_SLOTS) == 0, "b4 expired tickets filtered out");
        CHECK(nonce_issue(4, 1, d2, "confirm", &tf) == NR_OK, "b4 issue for invalidate check");
        nonce_invalidate_plugin(4);
        CHECK(nonce_list_pending(infos, NONCE_SLOTS) == 0,
              "b4 plugin invalidation empties the feed (unmount coherence)");
        nonce_invalidate_all();
    }

    printf("--- M3e S1b two-stage TTL (ISSUED 60s / APPROVED 10-min grace) ---\n");
    {
        NonceTicket ta, tb;
        unsigned long long t_issue;
        nonce_invalidate_all();
        g_fake_now += NONCE_APPROVED_TTL_MS + 2000;  /* clean slate */
        t_issue = g_fake_now;
        CHECK(nonce_issue(5, 0, d1, "destructive", &ta) == NR_OK, "S1b issue");
        g_fake_now = t_issue + 10000;
        CHECK(nonce_approve_txn(ta.txn) == NR_OK, "S1b approve at +10s");
        g_fake_now = t_issue + 70000;   /* past the OLD 60s issued-cap */
        CHECK(nonce_auto_pair_peek(5, 0, d1) == 1,
              "S1b peek at +70s post-issue sees APPROVED triple (bare-retry path)");
        CHECK(nonce_verify(ta.nonce, 5, 0, d1) == NR_OK,
              "S1b retry-with-nonce at +70s post-issue -> OK (six-ticket race fixed)");
        CHECK(nonce_consume(5, 0, d1, NULL, &tb) == NR_OK,
              "S1b auto-pair at +70s post-issue -> OK");
        CHECK(nonce_issue(5, 1, d1, "confirm", &ta) == NR_OK, "S1b issue #2");
        g_fake_now += 10000;
        CHECK(nonce_approve_txn(ta.txn) == NR_OK, "S1b approve #2");
        g_fake_now += NONCE_APPROVED_TTL_MS + 1000;   /* 10 min past approval */
        CHECK(nonce_auto_pair_peek(5, 1, d1) == 0,
              "S1b peek past grace cap -> no auto-pair candidate");
        CHECK(nonce_verify(ta.nonce, 5, 1, d1) == NR_EXPIRED,
              "S1b grace cap enforced: 10min+1s post-approval -> EXPIRED");
        CHECK(nonce_consume(5, 1, d1, NULL, &tb) == NR_EXPIRED,
              "S1b auto-pair past grace cap -> EXPIRED");
        CHECK(nonce_issue(5, 2, d1, "confirm", &ta) == NR_OK, "S1b issue #3 (unapproved)");
        g_fake_now += NONCE_TTL_MS + 1000;
        CHECK(nonce_verify(ta.nonce, 5, 2, d1) == NR_EXPIRED,
              "S1b ISSUED window unchanged: unapproved ticket still dies at 60s");
        nonce_invalidate_all();
    }

    printf("\n== nonce_test: %d passed, %d failed ==\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
