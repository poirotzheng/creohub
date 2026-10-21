/* creo-hub M2: reconcile implementation (docs\13 G3 / docs\15 R4).
 * Confluence acceptance (docs\15 DoD 12 / O10): after ANY dynamic history
 * (mount/unmount/toggle/rescan/corrupt-recovery in random order), the
 * quiescent committed set must equal the static assembly computed from the
 * final target view alone - verified by test\reconcile_test.c (>=1000
 * randomized sequences, harness reused in M3 with dependency scenarios). */
#include "reconcile.h"
#include "registry.h"
#include "hub_log.h"

#include <string.h>

#define TAG "RECON"

static int want_default(const PluginRec* p)
{
    /* M1 S10 restore-set rule: autoMount OR previous session ended ACTIVE */
    return (p->auto_mount ||
            (p->prev_state[0] && strcmp(p->prev_state, "ACTIVE") == 0)) ? 1 : 0;
}

void reconcile_target_refresh(void)
{
    int i, n = 0;
    for (i = 0; i < g_n_plugins; i++) {
        PluginRec* p = &g_plugins[i];
        if (!p->id[0]) continue;
        p->want_mount = want_default(p);
        p->want_known = 1;
        if (p->want_mount) n++;
    }
    hub_log(TAG, "target view refreshed from persisted semantics: %d/%d wanted",
            n, g_n_plugins);
}

int reconcile_set_target(int idx, int want)
{
    PluginRec* p;
    if (idx < 0 || idx >= g_n_plugins) return 0;
    p = &g_plugins[idx];
    if (!p->id[0]) return 0;
    p->want_mount = want ? 1 : 0;
    p->want_known = 1;
    hub_log(TAG, "target('%s') = %d (explicit edit)", p->id, p->want_mount);
    return 1;
}

int reconcile_run(ReconcileOrder order, int quiet, ReconcileResult* out)
{
    ReconcileResult r;
    int i;

    memset(&r, 0, sizeof(r));
    if (order != RECONCILE_ORDER_FLAT && order != RECONCILE_ORDER_TOPO) {
        hub_log(TAG, "order strategy %d unknown", (int)order);
        if (out) *out = r;
        return -1;
    }

    if (order == RECONCILE_ORDER_TOPO) {
        /* b3 (docs\22 8.1 / docs\23 3.5): two Kahn phases.
         * Phase A - drain-aware unmount: an ACTIVE want=0 record is ready when
         * no ACTIVE dependent remains; an ACTIVE dependent that still wants
         * mount BLOCKS (skipped, never cascaded behind the target view's back).
         * Phase B - topological mount: a want=1 REGISTERED/ERROR record is
         * ready when every resolved require is ACTIVE; a missing provider
         * leaves it REGISTERED with its waiting badge (timing-only deps constrain
         * activation timing only). */
        int doomed[MAX_PLUGINS];
        int attempted[MAX_PLUGINS];
        int progressed = 1;
        memset(doomed, 0, sizeof(doomed));
        memset(attempted, 0, sizeof(attempted));
        for (i = 0; i < g_n_plugins; i++) {
            PluginRec* p = &g_plugins[i];
            if (!p->id[0]) continue;
            if (!p->want_known) {           /* lazy default, same as FLAT path */
                p->want_mount = want_default(p);
                p->want_known = 1;
            }
        }
        /* Condemnation closure (b3, storm-found): an ACTIVE record is doomed
         * when want=0, when any resolved require is missing/dangling (provider
         * not ACTIVE), or when an ACTIVE provider is itself want=0/doomed.
         * The invariant ACTIVE => every dep ACTIVE plus confluence
         * (quiescent == static assembly, docs\22 invariant) FORCE the cascade: the
         * static side can never mount a dependent whose provider target is
         * off, so the quiescent side must drain it too. The earlier
         * "want=1 dependent blocks provider unmount" rule is REVOKED - it
         * made the two sides mathematically unequal. */
        do {
            progressed = 0;
            for (i = 0; i < g_n_plugins; i++) {
                PluginRec* p = &g_plugins[i];
                int j;
                if (!p->id[0] || !p->want_known) continue;
                if (p->state != PS_ACTIVE || doomed[i]) continue;
                if (!p->want_mount) { doomed[i] = 1; progressed = 1; continue; }
                for (j = 0; j < p->n_requires; j++) {
                    int v = p->req_idx[j];
                    if (v < 0 || doomed[v] || g_plugins[v].state != PS_ACTIVE ||
                        (g_plugins[v].want_known && !g_plugins[v].want_mount)) {
                        doomed[i] = 1; progressed = 1; break;
                    }
                }
            }
        } while (progressed);
        /* unmount doomed set dependent-first: a doomed record goes only when
         * no ACTIVE doomed dependent remains */
        progressed = 1;
        while (progressed) {
            progressed = 0;
            for (i = 0; i < g_n_plugins; i++) {
                int k, held = 0;
                if (!doomed[i] || attempted[i]) continue;
                if (g_plugins[i].state != PS_ACTIVE) { attempted[i] = 1; continue; }
                for (k = 0; k < g_n_plugins && !held; k++) {
                    int j;
                    if (k == i || !doomed[k] || g_plugins[k].state != PS_ACTIVE)
                        continue;
                    for (j = 0; j < g_plugins[k].n_requires; j++)
                        if (g_plugins[k].req_idx[j] == i) { held = 1; break; }
                }
                if (held) continue;
                attempted[i] = 1;
                {
                    ProError e = registry_unmount(i, quiet);
                    if (e == PRO_TK_NO_ERROR) { r.unmounted++; progressed = 1; }
                    else r.failed++;
                }
            }
        }
        memset(attempted, 0, sizeof(attempted));   /* phase B gets a fresh budget */
        progressed = 1;
        while (progressed) {
            progressed = 0;
            for (i = 0; i < g_n_plugins; i++) {
                PluginRec* p = &g_plugins[i];
                int j, ready = 1;
                if (!p->id[0] || !p->want_known) continue;
                if (!p->want_mount) continue;
                if (p->state != PS_REGISTERED && p->state != PS_ERROR) continue;
                if (attempted[i]) continue;         /* one attempt per run */
                for (j = 0; j < p->n_requires; j++) {
                    int v = p->req_idx[j];
                    if (v < 0 || g_plugins[v].state != PS_ACTIVE) { ready = 0; break; }
                }
                if (!ready) continue;              /* waiting badge already set */
                attempted[i] = 1;
                {
                    ProError e = registry_mount(i, quiet);
                    if (e == PRO_TK_NO_ERROR) { r.mounted++; progressed = 1; }
                    else r.failed++;
                }
            }
        }
        for (i = 0; i < g_n_plugins; i++) {         /* still-waiting records = skipped */
            PluginRec* p = &g_plugins[i];
            if (p->id[0] && p->want_mount && p->wait_reason[0] &&
                (p->state == PS_REGISTERED || p->state == PS_ERROR))
                r.skipped++;
            else if (p->id[0] && p->want_mount &&
                     (p->state == PS_INVALID || p->state == PS_UNSUPPORTED ||
                      p->state == PS_TAINTED))
                r.skipped++;                        /* bad graph/state never forced */
        }
        r.changed = r.mounted + r.unmounted;
        hub_log(TAG, "reconcile(topo): mounted=%d unmounted=%d failed=%d skipped=%d -> %s",
                r.mounted, r.unmounted, r.failed, r.skipped,
                r.changed ? "changed" : "quiescent");
        if (out) *out = r;
        return 0;
    }

    for (i = 0; i < g_n_plugins; i++) {
        PluginRec* p = &g_plugins[i];
        int want;
        if (!p->id[0]) continue;
        if (!p->want_known) {
            /* lazy default: new record after rescan, or pre-refresh startup */
            p->want_mount = want_default(p);
            p->want_known = 1;
        }
        want = p->want_mount;

        if (want) {
            switch (p->state) {
            case PS_REGISTERED:
            case PS_ERROR: {
                ProError e = registry_mount(i, quiet);
                if (e == PRO_TK_NO_ERROR) r.mounted++;
                else r.failed++;
                break;
            }
            case PS_ACTIVE:
                break;                      /* converged */
            default:
                r.skipped++;                /* INVALID/UNSUPPORTED/TAINTED/transient */
                break;
            }
        } else {
            switch (p->state) {
            case PS_ACTIVE: {
                ProError e = registry_unmount(i, quiet);
                if (e == PRO_TK_NO_ERROR) r.unmounted++;
                else r.failed++;
                break;
            }
            case PS_REGISTERED:
            case PS_ERROR:
            case PS_INVALID:
            case PS_UNSUPPORTED:
                break;                      /* converged (not mounted) */
            default:
                r.skipped++;                /* TAINTED/transient: never forced */
                break;
            }
        }
    }

    r.changed = r.mounted + r.unmounted;
    hub_log(TAG, "reconcile(flat): mounted=%d unmounted=%d failed=%d skipped=%d -> %s",
            r.mounted, r.unmounted, r.failed, r.skipped,
            r.changed ? "changed" : "quiescent");
    if (out) *out = r;
    return 0;
}
