/* creo-hub M2: reconcile - Cordis target/committed dual view (docs\13 G3,
 * docs\15 R4 pluggable ordering). External inputs (panel buttons, state file,
 * startup) ONLY edit the target view; committed state converges through
 * reconcile_run(). M2 = flat order strategy (no dependency graph); M3 adds
 * topological mount + reverse-topological drain-unmount over the same seam. */
#ifndef RECONCILE_H
#define RECONCILE_H

typedef enum {
    RECONCILE_ORDER_FLAT = 0,   /* M2: no requires graph, registry index order */
    RECONCILE_ORDER_TOPO = 1    /* M3: topological (rejected in M2, seam only) */
} ReconcileOrder;

typedef struct {
    int mounted;    /* successful mounts this run */
    int unmounted;  /* successful unmounts this run */
    int failed;     /* attempts that ended non-NO_ERROR (record state guards applied) */
    int skipped;    /* records neither converged nor actionable (INVALID/TAINTED/transient) */
    int changed;    /* mounted + unmounted; 0 => quiescent */
} ReconcileResult;

/* Rebuild the WHOLE target view from persisted semantics (startup path):
 * want = auto_mount || prev_state=="ACTIVE"  (the M1 S10 restore-set rule).
 * Call after state_load() at init; replaces registry_automount_all's selection
 * logic while registry_mount/unmount keep doing the guarded transitions. */
void reconcile_target_refresh(void);

/* Explicit target edit (panel/API mount/unmount buttons). Marks want known.
 * Returns 1 on success, 0 on bad idx/record. Does NOT drive the adapter -
 * call reconcile_run() afterwards (discipline: edit target, then converge). */
int reconcile_set_target(int idx, int want);

/* Drive committed (ACTIVE set) toward target (want set).
 * Guards: mount only from REGISTERED/ERROR; unmount only from ACTIVE;
 * INVALID/UNSUPPORTED/TAINTED/transient are skipped, never forced.
 * Records with want_known==0 get the lazy default (auto||prevACTIVE).
 * Returns 0 ok (incl. no-op), -1 on unsupported order strategy (TOPO in M2).
 * All transitions go through registry_mount/unmount (log+persist+popup rules
 * intact; quiet=1 for API-driven convergence, popups stay for menu path). */
int reconcile_run(ReconcileOrder order, int quiet, ReconcileResult* out);

#endif /* RECONCILE_H */
