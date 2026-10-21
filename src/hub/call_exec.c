/* creo-hub M3b b1: /call executor implementation. See call_exec.h.
 * Frozen response shapes: docs\22 §6.1. Two-layer error discipline (docs\24
 * §5): channel/infrastructure failures are 500 error:"channel:*" with
 * retryable:false; business failures are 200 + plugin OUT_JSON passthrough.
 * Win7 invariant: no new system DLL imports (no bcrypt yet - transactionId is
 * a NON-crypto correlation id; b2 nonce uses BCryptGenRandom per §6.3, which
 * adds bcrypt.dll - Vista+ stock, whitelisted in D-W7). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ProMdl.h>

#include "call_exec.h"
#include "adapter_ptk.h"
#include "schema_val.h"
#include "nonce.h"
#include "opmode.h"
#include "hub_log.h"
#include "cJSON.h"

#define TAG "CALL"

#define CALL_DEFAULT_TIMEOUT_MS 10000
#define CALL_MIN_TIMEOUT_MS     1000
#define CALL_MAX_TIMEOUT_MS     30000

/* hub-side change counter for the state token "regen" field (启示 8: cheap
 * version count so agents can decide "did my timed-out mutation land?".
 * Honest semantics: counts executed /call dispatches this hub session;
 * host regen status upgrade deferred to M3d live round). */
static unsigned long long g_change_seq = 0;

/* ------------------------------------------------------------ helpers */

static void ce_lit(char* out, int outsz, const char* text)
{
    strncpy(out, text, outsz - 1);
    out[outsz - 1] = '\0';
}

static void ce_json(char* out, int outsz, cJSON* r)
{
    char* txt = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!txt) {
        _snprintf(out, outsz, "{\"ok\":false,\"error\":\"response alloc failed\"}");
        out[outsz - 1] = '\0';
        return;
    }
    if ((int)strlen(txt) >= outsz) {
        cJSON_free(txt);
        _snprintf(out, outsz,
                  "{\"ok\":false,\"error\":\"result too large for envelope\","
                  "\"hint\":\"RESULT_CAP exceeded; shrink plugin output\"}");
        out[outsz - 1] = '\0';
        return;
    }
    strcpy(out, txt);
    cJSON_free(txt);
}

static cJSON* ce_err_obj(const char* error, const char* hint)
{
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 0);
    cJSON_AddStringToObject(r, "error", error);
    if (hint) cJSON_AddStringToObject(r, "hint", hint);
    return r;
}

static const char* ce_get_str(const cJSON* o, const char* key)
{
    const cJSON* j = cJSON_GetObjectItemCaseSensitive((cJSON*)o, key);
    return (j && cJSON_IsString(j)) ? j->valuestring : NULL;
}

/* 428 needConfirm body (frozen shape docs\22 §6.1 L258) - shared by the
 * first-call issuance, the consumed-reuse re-issue and dryRun. */
static void ce_need_confirm(char* resp, int respsz, const PluginCmd* cv,
                            const NonceTicket* t, const char* extra_hint)
{
    cJSON* r = ce_err_obj("confirmation required",
            extra_hint ? extra_hint :
            "human approval required: POST /api/v1/confirm {transactionId,approve:true} "
            "(panel/bridge), then retry with confirm_nonce");
    cJSON* nc = cJSON_AddObjectToObject(r, "needConfirm");
    char dh[17];
    nonce_digest_hex(t->digest, dh);
    cJSON_AddStringToObject(nc, "transactionId", t->txn);
    cJSON_AddStringToObject(nc, "nonce", t->nonce);
    cJSON_AddStringToObject(nc, "risk", cv->risk);
    cJSON_AddStringToObject(nc, "command", cv->id);
    cJSON_AddStringToObject(nc, "argsDigest", dh);
    cJSON_AddStringToObject(nc, "summary", cv->desc);
    cJSON_AddNumberToObject(nc, "expiresMs", (double)NONCE_TTL_MS);
    if (strcmp(cv->risk, "destructive") == 0)
        cJSON_AddStringToObject(nc, "confirmVia", "panel-required");
    *resp = '\0';
    ce_json(resp, respsz, r);
}

/* ------------------------------------------------------------ precheck */

int call_exec_precheck(const cJSON* body, CallTarget* tgt,
                       char* resp, int respsz, int* status)
{
    const char* plugin;
    const char* command;
    const cJSON* args;
    int idx, cidx = -1, i;
    PluginRec* p;
    PluginCmd* cv;
    cJSON* r;

    memset(tgt, 0, sizeof(*tgt));

    plugin  = ce_get_str(body, "plugin");
    command = ce_get_str(body, "command");
    if (!plugin || !command) {
        *status = 400;
        ce_json(resp, respsz, ce_err_obj("plugin and command are required",
                "body: {plugin, command, args?, timeoutMs?}"));
        return 0;
    }

    idx = registry_find_idx(plugin);
    if (idx < 0) {
        *status = 404;
        ce_json(resp, respsz, ce_err_obj("plugin not found",
                "check GET /api/v1/plugins for registered ids"));
        return 0;
    }
    p = &g_plugins[idx];

    for (i = 0; i < p->n_commands; i++) {
        if (strcmp(p->commands[i], command) == 0) { cidx = i; break; }
    }
    if (cidx < 0) {
        char hint[512];
        int off = 0;
        hint[0] = '\0';
        off += _snprintf(hint + off, sizeof(hint) - off, "plugin '%s' commands: ", p->id);
        if (off < 0 || off >= (int)sizeof(hint) - 2) off = (int)sizeof(hint) - 2;
        for (i = 0; i < p->n_commands; i++) {
            int w = _snprintf(hint + off, sizeof(hint) - off, "%s%s",
                              i ? ", " : "", p->commands[i]);
            if (w < 0 || w >= (int)sizeof(hint) - off) break;
            off += w;
        }
        if (p->n_commands == 0)
            _snprintf(hint + off, sizeof(hint) - off, "(none - v1 or command-less package)");
        hint[sizeof(hint) - 1] = '\0';
        *status = 404;
        ce_json(resp, respsz, ce_err_obj("command not found", hint));
        return 0;
    }
    cv = &p->cmdv[cidx];

    /* state gate: M3 simplification - ALWAYS require ACTIVE (docs\22 §6.1:
     * UNMOUNTING-window fine semantics deferred to M4). */
    if (p->state != PS_ACTIVE) {
        r = ce_err_obj("plugin not active", "mount the plugin first");
        cJSON_AddStringToObject(r, "state", registry_state_name(p->state));
        *status = 409;
        ce_json(resp, respsz, r);
        return 0;
    }

    /* schema value-domain (docs\22 §6.2 / docs\23 §3.2) */
    args = cJSON_GetObjectItemCaseSensitive((cJSON*)body, "args");
    if (args && !cJSON_IsObject(args) && !cJSON_IsNull(args)) {
        *status = 422;
        ce_json(resp, respsz, ce_err_obj("args must be a JSON object",
                "args is the command parameter object per inputSchema"));
        return 0;
    }
    {
        cJSON* schema = cJSON_Parse(cv->input_schema);
        char verr[512];
        if (schema) {
            verr[0] = '\0';
            if (!sv_validate_args(schema, (args && cJSON_IsObject(args)) ? args : NULL,
                                  verr, sizeof(verr))) {
                char path[256], reason[256];
                char* colon = strstr(verr, ": ");
                if (colon) {
                    int plen = (int)(colon - verr);
                    if (plen > (int)sizeof(path) - 1) plen = (int)sizeof(path) - 1;
                    memcpy(path, verr, plen); path[plen] = '\0';
                    strncpy(reason, colon + 2, sizeof(reason) - 1);
                    reason[sizeof(reason) - 1] = '\0';
                } else {
                    strncpy(path, "args", sizeof(path) - 1); path[sizeof(path) - 1] = '\0';
                    strncpy(reason, verr, sizeof(reason) - 1); reason[sizeof(reason) - 1] = '\0';
                }
                r = ce_err_obj("schema validation failed", verr);
                {
                    cJSON* details = cJSON_AddArrayToObject(r, "details");
                    cJSON* d = cJSON_CreateObject();
                    cJSON_AddStringToObject(d, "path", path);
                    cJSON_AddStringToObject(d, "reason", reason);
                    cJSON_AddItemToArray(details, d);
                }
                cJSON_Delete(schema);
                *status = 422;
                ce_json(resp, respsz, r);
                return 0;
            }
            cJSON_Delete(schema);
        }
        /* schema unparseable at runtime = registration-time bug; fail closed */
        else {
            *status = 500;
            ce_json(resp, respsz, ce_err_obj("command inputSchema corrupt at call time",
                    "registration should have rejected this manifest - report as hub bug"));
            return 0;
        }
    }

    /* b2 risk gate + dryRun (docs\22 §6.1/§6.3; docs\25 §2 b2).
     * safe = straight through (pure queries are not emissions). */
    {
        unsigned long long digest;
        const char* cn;
        const cJSON* jdry;
        digest = nonce_args_digest((args && cJSON_IsObject(args)) ? args : NULL);
        if (digest == 0) {
            /* canonicalization OOM (or true 0-digest ~2^-64): fail closed */
            *status = 500;
            ce_json(resp, respsz, ce_err_obj("args digest failed",
                      "canonical serialization OOM; retry with smaller args"));
            return 0;
        }

        jdry = cJSON_GetObjectItemCaseSensitive((cJSON*)body, "dryRun");
        if (jdry && cJSON_IsTrue(jdry)) {
            /* full chain but NEVER enqueued; answers wouldExecute and may
             * pre-issue the nonce (启示 10: saves the destructive first call
             * one wasted round trip). */
            int needs_confirm = (strcmp(cv->risk, "safe") != 0) &&
                                !opmode_allows(cv->risk) &&
                                !nonce_auto_pair_peek(idx, cidx, digest);
            char dh[17];
            nonce_digest_hex(digest, dh);
            r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "ok", 1);
            cJSON_AddBoolToObject(r, "dryRun", 1);
            {
                cJSON* we = cJSON_AddObjectToObject(r, "wouldExecute");
                cJSON_AddStringToObject(we, "plugin", p->id);
                cJSON_AddStringToObject(we, "command", cv->id);
                cJSON_AddStringToObject(we, "risk", cv->risk);
                cJSON_AddBoolToObject(we, "needsConfirm", needs_confirm);
                cJSON_AddStringToObject(we, "argsDigest", dh);
            }
            if (needs_confirm) {
                NonceTicket t;
                NonceResult nr = nonce_issue(idx, cidx, digest, cv->risk, &t);
                if (nr == NR_OK) {
                    cJSON* nc = cJSON_AddObjectToObject(r, "needConfirm");
                    cJSON_AddStringToObject(nc, "transactionId", t.txn);
                    cJSON_AddStringToObject(nc, "nonce", t.nonce);
                    cJSON_AddStringToObject(nc, "risk", cv->risk);
                    cJSON_AddStringToObject(nc, "command", cv->id);
                    cJSON_AddStringToObject(nc, "argsDigest", dh);
                    cJSON_AddStringToObject(nc, "summary", cv->desc);
                    cJSON_AddNumberToObject(nc, "expiresMs", (double)NONCE_TTL_MS);
                    if (strcmp(cv->risk, "destructive") == 0)
                        cJSON_AddStringToObject(nc, "confirmVia", "panel-required");
                    hub_log(TAG, "CONFIRM-ISSUE(dry) plugin=%s cmd=%s txn=%s nonce_id=%s",
                            p->id, cv->id, t.txn, t.nonce_id);
                } else {
                    cJSON_AddStringToObject(r, "nonceError", "table full - retry later");
                }
            }
            *status = 200;
            ce_json(resp, respsz, r);
            return 0;
        }

        if (strcmp(cv->risk, "safe") != 0) {
            if (opmode_allows(cv->risk)) {
                /* M3e Q1 operator mode menu (user-ratified): human armed
                 * M1/M2 from the panel; this risk auto-passes the nonce
                 * gate. Audited; mode never persists across hub restart. */
                hub_log(TAG, "CONFIRM-AUTOPASS mode=%d plugin=%s cmd=%s risk=%s",
                        opmode_current(), p->id, cv->id, cv->risk);
            } else {
            cn = ce_get_str(body, "confirm_nonce");
            if (cn && cn[0]) {
                NonceResult nr = nonce_verify(cn, idx, cidx, digest);
                char nid[12];
                strncpy(nid, cn, 8); nid[8] = '\0';
                if (nr == NR_OK) {
                    /* approved + triple-bound: pass; consumption happens at
                     * execute time (启示 11: never burn on enqueue). */
                    hub_log(TAG, "CONFIRM-PASS plugin=%s cmd=%s nonce_id=%s", p->id, cv->id, nid);
                } else if (nr == NR_ARGS_MISMATCH) {
                    *status = 409;
                    r = ce_err_obj("NONCE_ARGS_MISMATCH",
                            "approved args differ from submitted args (argsDigest binding); "
                            "re-confirm with the exact approved arguments");
                    cJSON_AddStringToObject(r, "nonce_id", nid);
                    ce_json(resp, respsz, r);
                    return 0;
                } else if (nr == NR_CONSUMED) {
                    /* replay of a burned nonce: re-issue fresh (docs\22 §6.3:
                     * new nonce still needs human approval = no new risk) */
                    NonceTicket t;
                    if (nonce_issue(idx, cidx, digest, cv->risk, &t) != NR_OK) {
                        *status = 503;
                        ce_json(resp, respsz, ce_err_obj("confirmation table full",
                                  "all nonce slots hold live approvals; retry shortly"));
                        return 0;
                    }
                    hub_log(TAG, "CONFIRM-REISSUE(consumed) plugin=%s cmd=%s old_nonce_id=%s txn=%s nonce_id=%s",
                            p->id, cv->id, nid, t.txn, t.nonce_id);
                    ce_need_confirm(resp, respsz, cv, &t, "previous nonce consumed; "
                            "human approval required again for the fresh nonce");
                    *status = 428;
                    return 0;
                } else if (nr == NR_PENDING_APPROVAL) {
                    *status = 409;
                    r = ce_err_obj("NONCE_PENDING_APPROVAL",
                            "nonce issued but human approval not registered yet; "
                            "approve via panel / POST /api/v1/confirm, then retry with the same nonce");
                    cJSON_AddStringToObject(r, "nonce_id", nid);
                    ce_json(resp, respsz, r);
                    return 0;
                } else {
                    /* NR_EXPIRED / NR_NOT_FOUND: same client fix - re-issue */
                    *status = 409;
                    r = ce_err_obj("NONCE_EXPIRED",
                            "nonce unknown, evicted or past its TTL "
                            "(60s issued window / 10min approved grace, M3e S1b); "
                            "call again WITHOUT confirm_nonce to receive a fresh one");
                    cJSON_AddStringToObject(r, "nonce_id", nid);
                    ce_json(resp, respsz, r);
                    return 0;
                }
            } else if (nonce_auto_pair_peek(idx, cidx, digest)) {
                /* L3 panel auto-pair: human approved in-panel within the
                 * APPROVED grace window (M3e S1b: 10 min from approval) with
                 * the same (plugin,command,argsDigest); nonce stays inside
                 * the hub/panel loop. Consumed at execute time. */
                hub_log(TAG, "CONFIRM-AUTOPAIR plugin=%s cmd=%s", p->id, cv->id);
            } else {
                NonceTicket t;
                NonceResult nr = nonce_issue(idx, cidx, digest, cv->risk, &t);
                if (nr != NR_OK) {
                    *status = 503;
                    ce_json(resp, respsz, ce_err_obj("confirmation table full",
                              "all nonce slots hold live approvals; retry shortly"));
                    return 0;
                }
                hub_log(TAG, "CONFIRM-ISSUE plugin=%s cmd=%s txn=%s nonce_id=%s",
                        p->id, cv->id, t.txn, t.nonce_id);
                ce_need_confirm(resp, respsz, cv, &t, NULL);
                *status = 428;
                return 0;
            }
            }
        }
    }

    strncpy(tgt->plugin, plugin, sizeof(tgt->plugin) - 1);
    strncpy(tgt->command, command, sizeof(tgt->command) - 1);
    tgt->cidx = cidx;
    tgt->idempotent = cv->idempotent;
    tgt->is_async = cv->is_async;              /* M4b */
    strncpy(tgt->risk, cv->risk, sizeof(tgt->risk) - 1);
    tgt->risk[sizeof(tgt->risk) - 1] = '\0';
    tgt->timeout_ms = cv->timeout_sec > 0 ? cv->timeout_sec * 1000
                                          : CALL_DEFAULT_TIMEOUT_MS;
    if (tgt->timeout_ms < CALL_MIN_TIMEOUT_MS) tgt->timeout_ms = CALL_MIN_TIMEOUT_MS;
    if (tgt->timeout_ms > CALL_MAX_TIMEOUT_MS) tgt->timeout_ms = CALL_MAX_TIMEOUT_MS;
    return 1;
}

/* ------------------------------------------------------------ execute */

static void ce_state_token(cJSON* r)
{
    cJSON* st = cJSON_AddObjectToObject(r, "state");
    ProMdl mdl = NULL;
    g_change_seq++;
    if (ProMdlCurrentGet(&mdl) == PRO_TK_NO_ERROR && mdl) {
        ProName wname;
        wname[0] = L'\0';
        if (ProMdlNameGet(mdl, wname) == PRO_TK_NO_ERROR) {
            char* name = adapter_w_to_utf8(wname);
            if (name) {
                cJSON_AddStringToObject(st, "model", name);
                free(name);
            } else {
                cJSON_AddNullToObject(st, "model");
            }
        } else {
            cJSON_AddNullToObject(st, "model");
        }
    } else {
        cJSON_AddNullToObject(st, "model");
    }
    cJSON_AddNumberToObject(st, "regen", (double)g_change_seq);
}

int call_exec_execute(const char* payload, char* out, int outsz,
                      char* idout, char* stateout)
{
    cJSON* body;
    cJSON* args;
    cJSON* fwd;
    cJSON* ctx;
    cJSON* r;
    const char* plugin;
    const char* command;
    int idx, cidx = -1, i;
    PluginRec* p;
    PluginCmd* cv = NULL;
    char* in_json = NULL;
    char* out_json = NULL;
    ProArgument* inputs = NULL;
    ProArgument* outputs = NULL;
    ProError fret = PRO_TK_NO_ERROR;
    AdapterChannelStatus ch;
    char chbuf[64];
    char label_seen[64];
    int n_out = 0;
    ULONGLONG t0, ms;
    const char* route;
    const char* func;
    int budget;
    char txn_a[32];
    char nid_a[12];

    strcpy(txn_a, "-");
    strcpy(nid_a, "-");
    idout[0] = '\0';
    stateout[0] = '\0';

    body = cJSON_Parse(payload);
    if (!body) {
        strcpy(stateout, "bad-json");
        ce_lit(out, outsz, "{\"ok\":false,\"error\":\"bad json\"}");
        return 400;
    }
    plugin  = ce_get_str(body, "plugin");
    command = ce_get_str(body, "command");
    args    = cJSON_GetObjectItemCaseSensitive(body, "args");
    if (!plugin || !command) {
        cJSON_Delete(body);
        strcpy(stateout, "bad-shape");
        ce_lit(out, outsz, "{\"ok\":false,\"error\":\"plugin and command are required\"}");
        return 400;
    }

    /* TOCTOU re-resolution: the queue may have held this envelope while the
     * registry moved on (unmount/rescan). Never trust precheck's indexes. */
    idx = registry_find_idx(plugin);
    if (idx < 0) {
        cJSON_Delete(body);
        strcpy(stateout, "plugin-vanished");
        ce_json(out, outsz, ce_err_obj("plugin not found", "state changed while queued"));
        return 404;
    }
    p = &g_plugins[idx];
    strcpy(idout, p->id);
    for (i = 0; i < p->n_commands; i++)
        if (strcmp(p->commands[i], command) == 0) { cidx = i; break; }
    if (cidx < 0) {
        cJSON_Delete(body);
        _snprintf(stateout, 96, "cmd-vanished %s", command);
        ce_json(out, outsz, ce_err_obj("command not found", "state changed while queued"));
        return 404;
    }
    cv = &p->cmdv[cidx];
    if (p->state != PS_ACTIVE) {
        cJSON_Delete(body);
        r = ce_err_obj("plugin not active", "state changed while queued");
        cJSON_AddStringToObject(r, "state", registry_state_name(p->state));
        _snprintf(stateout, 96, "%s(queued)", registry_state_name(p->state));
        ce_json(out, outsz, r);
        return 409;
    }
    budget = cv->timeout_sec > 0 ? cv->timeout_sec * 1000 : CALL_DEFAULT_TIMEOUT_MS;

    /* b2: atomic nonce consumption AT DEQUEUE (启示 11 / docs\22 §6.3):
     * never burned at enqueue for an execution that then fails. Race window
     * (two envelopes passed precheck with the same nonce) resolves here -
     * the second one gets 409 NONCE_CONSUMED.
     * M3e Q1: operator-mode auto-pass skips consumption (no slot exists);
     * a mode disarmed while queued makes consume fail -> 409 fail-closed. */
    if (strcmp(cv->risk, "safe") != 0 && !opmode_allows(cv->risk)) {
        const char* cn = ce_get_str(body, "confirm_nonce");
        unsigned long long digest =
            nonce_args_digest((args && cJSON_IsObject(args)) ? args : NULL);
        NonceTicket t;
        NonceResult nr = nonce_consume(idx, cidx, digest, cn, &t);
        if (nr != NR_OK) {
            const char* code =
                nr == NR_ARGS_MISMATCH     ? "NONCE_ARGS_MISMATCH" :
                nr == NR_CONSUMED          ? "NONCE_CONSUMED" :
                nr == NR_PENDING_APPROVAL  ? "NONCE_PENDING_APPROVAL" :
                                             "NONCE_EXPIRED";
            cJSON_Delete(body);
            r = ce_err_obj(code,
                    "confirmation state changed while queued; "
                    "call again WITHOUT confirm_nonce to re-confirm");
            _snprintf(stateout, 160, "cmd=%s nonce-reject=%s", command, code);
            hub_log(TAG, "NONCE-REJECT plugin=%s cmd=%s code=%s", p->id, command, code);
            ce_json(out, outsz, r);
            return 409;
        }
        strncpy(txn_a, t.txn, sizeof(txn_a) - 1);   txn_a[sizeof(txn_a) - 1] = '\0';
        strncpy(nid_a, t.nonce_id, sizeof(nid_a) - 1); nid_a[sizeof(nid_a) - 1] = '\0';
    }

    /* forwarded IN_JSON: SDK protocol {"command","args"} + v1 ctx (docs\23
     * §5.2: timeout budget for cooperative self-check; SDK strips _ctx). */
    fwd = cJSON_CreateObject();
    cJSON_AddStringToObject(fwd, "command", command);
    if (args && cJSON_IsObject(args))
        cJSON_AddItemToObject(fwd, "args", cJSON_Duplicate(args, 1));
    ctx = cJSON_AddObjectToObject(fwd, "_ctx");
    cJSON_AddNumberToObject(ctx, "budget_ms", budget);
    cJSON_AddNumberToObject(ctx, "sdk_version", 2);   /* M4b: SDK v2 (progress) */
    {   /* M4b async: hub-created progress slot name (M4a sdk\hub_jobslot.h) */
        cJSON* jslot = cJSON_GetObjectItemCaseSensitive(body, "_job_slot");
        if (cJSON_IsString(jslot) && jslot->valuestring && jslot->valuestring[0])
            cJSON_AddStringToObject(ctx, "progress_map", jslot->valuestring);
    }
    in_json = cJSON_PrintUnformatted(fwd);
    cJSON_Delete(fwd);
    if (!in_json) {
        cJSON_Delete(body);
        strcpy(stateout, "fwd-alloc");
        ce_json(out, outsz, ce_err_obj("channel:ALLOC", "forwarded payload serialization failed"));
        return 500;
    }

    route = p->call_route[0] ? p->call_route : "B";
    func  = p->hub_cmd_export[0] ? p->hub_cmd_export : "hub_cmd";

    inputs = adapter_build_inputs(in_json, 0);
    if (!inputs) {
        cJSON_free(in_json);
        cJSON_Delete(body);
        strcpy(stateout, "input-build");
        ce_json(out, outsz, ce_err_obj("channel:ALLOC", "ProArgument input build failed"));
        return 500;
    }

    t0 = GetTickCount64();
    ch = adapter_call(p, route, func, inputs, &outputs, &fret, chbuf, sizeof(chbuf));
    ms = GetTickCount64() - t0;

    if (ch != ADP_CH_OK) {
        hub_log(TAG, "INFRA-FAIL plugin=%s cmd=%s route=%s channel=%s func_ret=%d(%s) ms=%llu",
                p->id, command, route, chbuf, (int)fret, hub_errname(fret), ms);
        _snprintf(stateout, 96, "channel=%s ms=%llu", chbuf, ms);
        cJSON_free(in_json);
        cJSON_Delete(body);
        ProArgumentProarrayFree(&inputs);
        if (outputs) ProArgumentProarrayFree(&outputs);
        r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 0);
        {
            char ebuf[96];
            _snprintf(ebuf, sizeof(ebuf), "channel:%s", chbuf);
            ebuf[sizeof(ebuf) - 1] = '\0';
            cJSON_AddStringToObject(r, "error", ebuf);
        }
        cJSON_AddBoolToObject(r, "retryable", 0);
        cJSON_AddNumberToObject(r, "ms", (double)ms);
        cJSON_AddStringToObject(r, "hint",
                "infrastructure failure (not a business result); check plugin mount state and hubCmdExport");
        ce_json(out, outsz, r);
        return 500;
    }

    out_json = adapter_extract_out_json(outputs, &n_out, label_seen, sizeof(label_seen));
    ProArgumentProarrayFree(&inputs);
    if (outputs) ProArgumentProarrayFree(&outputs);
    cJSON_free(in_json);

    if (!out_json) {
        cJSON_Delete(body);
        _snprintf(stateout, 96, "cmd=%s no-OUT_JSON ms=%llu", command, ms);
        r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 0);
        cJSON_AddStringToObject(r, "error", "plugin returned no OUT_JSON");
        cJSON_AddBoolToObject(r, "retryable", 0);
        cJSON_AddNumberToObject(r, "ms", (double)ms);
        cJSON_AddStringToObject(r, "hint", "hub_sdk v0+ always emits OUT_JSON; hand-rolled entries must set it");
        ce_json(out, outsz, r);
        return 500;
    }

    r = cJSON_Parse(out_json);
    free(out_json);
    if (!r) {
        cJSON_Delete(body);
        _snprintf(stateout, 96, "cmd=%s out-not-json ms=%llu", command, ms);
        ce_json(out, outsz, ce_err_obj("plugin OUT_JSON not parseable",
                  "SDK resp builders guarantee valid JSON; hand-rolled entries must too"));
        return 500;
    }
    cJSON_Delete(body);

    {
        const cJSON* okf = cJSON_GetObjectItemCaseSensitive(r, "ok");
        int biz_fail = (okf && cJSON_IsFalse(okf));
        if (biz_fail) {
            /* 200 business failure passthrough (§6.1): plugin-shaped err_code/
             * err_msg/hint stay authoritative; hub adds transport facts only. */
            cJSON_AddNumberToObject(r, "ms", (double)ms);
            _snprintf(stateout, 160, "cmd=%s risk=%s txn=%s nonce_id=%s biz-fail ms=%llu",
                      command, cv->risk, txn_a, nid_a, ms);
            hub_log(TAG, "BIZ-FAIL plugin=%s cmd=%s txn=%s nonce_id=%s ms=%llu func_ret=%d(%s)",
                    p->id, command, txn_a, nid_a, ms, (int)fret, hub_errname(fret));
            ce_json(out, outsz, r);
            return 200;
        }
    }

    /* 200 success: {ok:true, result:<OUT_JSON raw>, ms, state:{model,regen}} */
    {
        cJSON* wrap = cJSON_CreateObject();
        cJSON_AddBoolToObject(wrap, "ok", 1);
        cJSON_AddItemToObject(wrap, "result", r);
        cJSON_AddNumberToObject(wrap, "ms", (double)ms);
        ce_state_token(wrap);
        _snprintf(stateout, 160, "cmd=%s risk=%s txn=%s nonce_id=%s result=ok ms=%llu",
                  command, cv->risk, txn_a, nid_a, ms);
        hub_log(TAG, "OK plugin=%s cmd=%s route=%s txn=%s nonce_id=%s ms=%llu func_ret=%d(%s)",
                p->id, command, route, txn_a, nid_a, ms, (int)fret, hub_errname(fret));
        ce_json(out, outsz, wrap);
        return 200;
    }
}
