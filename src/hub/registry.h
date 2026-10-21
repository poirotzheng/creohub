/* creo-hub M1: plugin registry records + state machine facade */
#ifndef REGISTRY_H
#define REGISTRY_H

#include <windows.h>
#include <ProToolkit.h>
#include <ProToolkitDll.h>

#define MAX_PLUGINS 64

/* M2 (docs\13 G2 / docs\15 R1): v2 command contract - parsed and passed
 * through to /api/v1/manifest + panel command face, NOT enforced in M2
 * (schema hard-validation at call time / risk gate / requires reconciliation
 * are M3). risk is validated at parse time (R1) to safe|confirm|destructive.
 * M3b b1: schema value-domain enforced at call time (call_exec + schema_val);
 * context field added (docs\24 §9 boom-scope decision, P-b1-1 default "any"). */
typedef struct {
    char id[64];
    char title[96];
    char desc[384];
    char desc_llm[384];         /* descForLLM */
    char risk[12];              /* safe|confirm|destructive (required in v2) */
    char input_schema[1024];    /* raw JSON text, M2 allowed subset (R1) */
    char context[12];           /* M3b: part|assembly|any (default any) */
    int  reversible;            /* -1 unspecified, 0 false, 1 true */
    int  idempotent;            /* -1 unspecified, 0 false, 1 true */
    int  timeout_sec;           /* 0 = unspecified (queue default applies) */
    /* M4b (docs\39 3.1/3.5/3.6): additive contract fields (v2 optional) */
    int  is_async;              /* 1 = long task: /call returns 202 + jobId */
    char undo_cmd[64];          /* undoCommand: '<pkg>.<cmd>' same package ("" = none) */
    char group[33];             /* group: tool grouping / token economy ("" = misc) */
} PluginCmd;

typedef enum {
    PS_INVALID = 0,     /* manifest bad, cannot mount */
    PS_UNSUPPORTED,     /* adapter not ptk-sync (M1) */
    PS_REGISTERED,      /* valid, not mounted */
    PS_MOUNTING,        /* transient */
    PS_ACTIVE,          /* mounted */
    PS_UNMOUNTING,      /* transient */
    PS_ERROR,           /* last mount failed, retryable */
    PS_TAINTED          /* unrecoverable this session, restart Creo */
} PluginState;

typedef struct {
    char id[64];
    char sid[64];               /* sanitized id for action/msg keys ('-'/'.' -> '_') */
    char name[128];
    char version[32];
    char adapter[32];
    char dir[MAX_PATH];         /* absolute plugin package dir */
    char entry_abs[MAX_PATH];   /* absolute dll path */
    char text_abs[MAX_PATH];    /* absolute textDir */
    char commands[16][64];      /* command ids (namespace-checked) */
    int  n_commands;
    PluginCmd cmdv[16];         /* M2 G2: rich v2 command info, parallel to commands[] */
    int  manifest_version;      /* M2 G2: 1 (M1 contract) or 2 (v2 contract) */
    unsigned long long fingerprint; /* M2 R2: FNV-1a64(plugin.json bytes + entry_abs) */
    int  fingerprint_changed;   /* M2 R2: ACTIVE && manifest changed -> badge, no auto-remount */
    char requires_list[8][64];  /* M2 G2: parsed only; dependency graph execution = M3b b3 */
    int  n_requires;
    char provides_list[8][64];  /* M2 G2/P3: parsed + exposed; service registry = M3+ */
    int  n_provides;
    int  req_idx[8];            /* b3: requires resolved to record idx (-1 = missing) */
    char wait_reason[96];       /* b3: badge "waiting for: <id>" while a dep is absent */
    int  cycle_member;          /* b3: scan-time cycle detection mark (-> INVALID) */
    char current_version[32];   /* b3 P1: version pointer value ("" = classic layout) */
    int  auto_mount;            /* effective value (manifest + state override) */
    int  user_display;
    char hub_cmd_export[64];    /* M3b b1: v2 hubCmdExport (default "hub_cmd") */
    char call_route[2];         /* M3b b1: v2 callRoute "B" (default) | "A" */
    char invalid_reason[256];
    char prev_state[32];        /* lastState read from hub_state.json (display) */
    PluginState state;
    ProError last_err;
    ProError last_user_err;
    ProPath  last_msg;
    ProToolkitDllHandle handle; /* self-maintained (M0: HandleGet unreliable) */
    int mounted_once;
    /* M2 (docs\13 G3 / docs\15 R4): target view.
     * want_mount = session-authoritative target; want_known==0 means "not yet
     * materialized" -> reconcile lazily defaults it to (auto_mount || prevACTIVE),
     * i.e. the M1 S10 restore-set rule. External inputs (panel buttons, state
     * file, startup) only edit target; committed state converges via reconcile(). */
    int want_mount;
    int want_known;
} PluginRec;

extern PluginRec g_plugins[MAX_PLUGINS];
extern int g_n_plugins;

const char* registry_state_name(PluginState s);
int  registry_scan(const char* root);      /* fresh scan of <root>\plugins\* */
int  registry_rescan(const char* root);    /* rescan, carry ACTIVE/TAINTED runtime state */
PluginRec* registry_find(const char* id);
int  registry_find_idx(const char* id);    /* index into g_plugins, or -1 */

/* guarded ops: transition + adapter call + log + persist + optional popup */
ProError registry_mount(int idx, int quiet);
ProError registry_unmount(int idx, int quiet);

/* b3 depgraph (docs\22 8.1 / docs\23 3.5): resolve requires to record indices
 * (plugin id first, then provides-service), detect cycles at scan time
 * (members -> INVALID + "dependency cycle: a->b->a"), set waiting badges for
 * missing deps (record stays REGISTERED). Called at end of scan/rescan. */
void registry_depgraph_refresh(void);
/* Drain-unmount plan: transitive ACTIVE/registered dependents closure of idx
 * in reverse-topological order (deepest dependent first, idx last). Returns
 * count filled into order[] (<= max_order); 0 = nothing to drain but idx
 * itself is still returned as the single element when idx is a valid record. */
int registry_drain_plan(int idx, int* order, int max_order);
/* Execute the drain: unmount dependents (reverse topo) then idx; stops at the
 * first failure and preserves the scene (returns that ProError). */
ProError registry_unmount_drain(int idx, int quiet);
/* mount every plugin with effective autoMount==true and state REGISTERED/ERROR.
 * returns attempted count; ok/fail via out params. */
int registry_automount_all(int quiet, int* ok_out, int* fail_out);

/* M2 (docs\09 2.5): autoMount switch edit = target edit + persist.
 * Sets auto_mount AND want_mount=value (panel toggle semantics: off = do not
 * want mounted this session; next Creo start rebuilds target from this flag),
 * then state_save. Returns 1 on success. */
int registry_set_automount(int idx, int value);

#endif /* REGISTRY_H */
