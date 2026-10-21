/* =====================================================================
 * creo-hub M2 OFFLINE test: reconcile (docs\13 G3 target/committed dual
 * view + docs\15 R4 pluggable-order seam) and the CONFLUENCE STORM
 * harness (docs\15 DoD 12 / gate O10):
 *   >=1000 randomized operation sequences (target edits / automount
 *   toggles / direct menu-style mount+unmount / rescan / restart /
 *   corrupt-state recovery / save) -> quiesce -> the committed ACTIVE
 *   set must equal the STATIC ASSEMBLY computed from the final target
 *   view alone ("dynamic history leaves no trace").
 * Hermetic: runs against a synthetic plugin tree under %TEMP%, the real
 * <REPO_ROOT> state files are never touched. No Creo, no PTK libs.
 * Exit code = number of failures (0 = ALL PASS).
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub_log.h"
#include "registry.h"
#include "state.h"
#include "reconcile.h"
#include "version_mgr.h"

extern int g_hub_log_quiet;   /* defined in test_stubs.c */

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

#define NSTORM      1000
#define MAXOPS      40
#define NPKGS       6

static char g_troot[MAX_PATH];
static const char* g_ids[NPKGS] = {
    "p-alpha", "p-beta", "p-gamma", "p-delta", "p-fail", "p-bad"
};

/* ---------------------------------------------------------- fs helpers */

static void write_file(const char* path, const char* content)
{
    FILE* f = fopen(path, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    char* p;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 3; *p; p++) {          /* skip "X:\" */
        if (*p == '\\') {
            *p = '\0';
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

/* kind: 0 normal, 1 fail-entry (stub rejects names containing demo_fail),
 *       2 bad manifest (id missing -> INVALID, id falls back to dirname) */
static void mkpkg(const char* root, const char* id, int automount, int kind)
{
    char dir[MAX_PATH], sub[MAX_PATH], json[MAX_PATH * 2 + 512];
    const char* entryname = (kind == 1) ? "demo_fail.dll" : "bin.dll";

    _snprintf(dir, sizeof(dir), "%s\\plugins\\%s", root, id);
    _snprintf(sub, sizeof(sub), "%s\\bin", dir);            mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\text\\text\\usascii", dir); mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\bin\\%s", dir, (kind == 1) ? "demo_fail.dll" : id);
    {   /* entry file: stub never loads it, existence is all that matters */
        char epath[MAX_PATH];
        _snprintf(epath, sizeof(epath), "%s\\bin\\%s", dir, entryname);
        write_file(epath, "MZ-fake-dll-for-offline-test");
        (void)sub;
    }
    _snprintf(sub, sizeof(sub), "%s\\text\\text\\usascii\\%s_messages.txt", dir, id);
    write_file(sub, "#MSG dummy\n");

    if (kind == 2) {
        _snprintf(json, sizeof(json),
                 "%s\\plugin.json", dir);
        write_file(json,
            "{\"manifestVersion\":1,\"adapter\":\"ptk-sync\","
            "\"entry\":\"bin/bin.dll\",\"textDir\":\"text\"}");   /* no id */
        return;
    }
    _snprintf(json, sizeof(json), "%s\\plugin.json", dir);
    {
        char body[MAX_PATH * 2 + 512];
        _snprintf(body, sizeof(body),
            "{\"manifestVersion\":1,\"id\":\"%s\",\"name\":\"%s pkg\",\"version\":\"1.0.0\","
            "\"adapter\":\"ptk-sync\",\"entry\":\"bin/%s\",\"textDir\":\"text\""
            "%s,\"commands\":[{\"id\":\"%s.one\"}]}",
            id, id, entryname, automount ? ",\"autoMount\":true" : "", id);
        write_file(json, body);
    }
}

/* b3: rewrite a package as v2 manifest WITH requires (storm dependency graph) */
static void set_requires_v2(const char* id, int automount, const char* req_csv)
{
    char json[MAX_PATH], body[1024];
    _snprintf(json, sizeof(json), "%s\\plugins\\%s\\plugin.json", g_troot, id);
    _snprintf(body, sizeof(body),
        "{\"manifestVersion\":2,\"id\":\"%s\",\"name\":\"%s pkg\",\"version\":\"1.0.0\","
        "\"adapter\":\"ptk-sync\",\"entry\":\"bin/bin.dll\",\"textDir\":\"text\"%s,"
        "\"requires\":[%s],"
        "\"commands\":[{\"id\":\"%s.one\",\"title\":\"t\",\"desc\":\"d\","
        "\"descForLLM\":\"d\",\"risk\":\"safe\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}]",
        id, id, automount ? ",\"autoMount\":true" : "", req_csv, id);
    write_file(json, body);
}

/* b3 P1: immutable version dirs for p-alpha (pointer package in the storm) */
static void mkver_alpha(const char* ver)
{
    char d[MAX_PATH], f[MAX_PATH], body[1024];
    _snprintf(d, sizeof(d), "%s\\plugins\\p-alpha\\versions\\%s\\bin", g_troot, ver);
    mkdir_p(d);
    _snprintf(d, sizeof(d), "%s\\plugins\\p-alpha\\versions\\%s\\text\\text\\usascii", g_troot, ver);
    mkdir_p(d);
    _snprintf(f, sizeof(f), "%s\\plugins\\p-alpha\\versions\\%s\\bin\\bin.dll", g_troot, ver);
    write_file(f, "MZ-fake-dll-for-offline-test");
    _snprintf(f, sizeof(f), "%s\\plugins\\p-alpha\\versions\\%s\\text\\text\\usascii\\p-alpha_messages.txt", g_troot, ver);
    write_file(f, "#MSG dummy\n");
    _snprintf(f, sizeof(f), "%s\\plugins\\p-alpha\\versions\\%s\\plugin.json", g_troot, ver);
    _snprintf(body, sizeof(body),
        "{\"manifestVersion\":2,\"id\":\"p-alpha\",\"name\":\"alpha pkg\",\"version\":\"%s\","
        "\"adapter\":\"ptk-sync\",\"entry\":\"bin/bin.dll\",\"textDir\":\"text\","
        "\"autoMount\":true,"
        "\"commands\":[{\"id\":\"p-alpha.one\",\"title\":\"t\",\"desc\":\"d\","
        "\"descForLLM\":\"d\",\"risk\":\"safe\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}]",
        ver);
    write_file(f, body);
}

static void setup_root(void)
{
    char st[MAX_PATH + 32];
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    tmp[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_storm", tmp);
    mkdir_p(g_troot);
    mkpkg(g_troot, "p-alpha", 1, 0);
    mkpkg(g_troot, "p-beta",  0, 0);
    mkpkg(g_troot, "p-gamma", 0, 0);
    mkpkg(g_troot, "p-delta", 1, 0);
    mkpkg(g_troot, "p-fail",  0, 1);
    mkpkg(g_troot, "p-bad",   0, 2);
    /* b3: dependency edges + P1 version dirs enter the storm */
    set_requires_v2("p-beta", 0, "\"p-alpha\"");
    set_requires_v2("p-gamma", 0, "\"p-alpha\"");
    mkver_alpha("1.0.0");
    mkver_alpha("1.1.0");
    {   /* thin pointer root for p-alpha */
        char json[MAX_PATH];
        _snprintf(json, sizeof(json), "%s\\plugins\\p-alpha\\plugin.json", g_troot);
        write_file(json,
            "{\"manifestVersion\":2,\"id\":\"p-alpha\",\"adapter\":\"ptk-sync\","
            "\"currentVersion\":\"1.0.0\"}");
    }
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
    _snprintf(st, sizeof(st), "%s\\hub_state.json.tmp", g_troot);
    remove(st);
}

static void remove_state_file(void)
{
    char st[MAX_PATH + 32];
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
}

static int idx_of(const char* id) { return registry_find_idx(id); }

/* ------------------------------------------------------------ fixtures */

static void fixture_checks(void)
{
    ReconcileResult r;
    int ia, ib, id_, ipf, ipb;

    printf("--- R fixtures: reconcile target/committed semantics ---\n");
    strcpy(g_root, g_troot);
    remove_state_file();
    registry_scan(g_troot);

    ia  = idx_of("p-alpha"); ib = idx_of("p-beta");
    id_ = idx_of("p-delta"); ipf = idx_of("p-fail"); ipb = idx_of("p-bad");
    CHECK(g_n_plugins == NPKGS, "R1 scan finds 6 synthetic packages");
    CHECK(ia >= 0 && g_plugins[ia].auto_mount == 1 && g_plugins[ia].state == PS_REGISTERED,
          "R1 p-alpha REGISTERED autoMount=1");
    CHECK(ipb >= 0 && g_plugins[ipb].state == PS_INVALID, "R1 p-bad INVALID (id missing)");
    CHECK(ipf >= 0 && g_plugins[ipf].state == PS_REGISTERED, "R1 p-fail REGISTERED (fail injected at mount)");

    reconcile_target_refresh();
    CHECK(g_plugins[ia].want_mount == 1 && g_plugins[id_].want_mount == 1 &&
          g_plugins[ib].want_mount == 0,
          "R2 startup refresh: want = autoMount set (alpha,delta)");

    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(r.mounted == 2 && r.changed == 2 && r.unmounted == 0,
          "R3 first reconcile mounts exactly the wanted 2");
    CHECK(g_plugins[ia].state == PS_ACTIVE && g_plugins[id_].state == PS_ACTIVE,
          "R3 alpha+delta ACTIVE (committed == target)");
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(r.changed == 0, "R3 second reconcile quiescent (idempotent)");

    /* R4: explicit session unmount of an autoMount plugin */
    reconcile_set_target(ia, 0);
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(r.unmounted == 1 && g_plugins[ia].state == PS_REGISTERED,
          "R4 explicit target-off unmounts alpha this session");
    CHECK(g_plugins[ia].auto_mount == 1, "R4 autoMount flag itself untouched by target edit");
    /* restart cycle: scan + load + refresh -> autoMount drives target again */
    state_save(g_troot);
    registry_scan(g_troot);
    state_load(g_troot);
    reconcile_target_refresh();
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(g_plugins[idx_of("p-alpha")].state == PS_ACTIVE,
          "R4 restart remounts alpha (autoMount=true target rebuilt)");

    /* R5: automount switch off = persisted target off, survives restart */
    ia = idx_of("p-alpha");
    registry_set_automount(ia, 0);
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(g_plugins[ia].state == PS_REGISTERED && g_plugins[ia].auto_mount == 0,
          "R5 automount(0) unmounts via reconcile + clears flag");
    {
        char buf[2048]; size_t rd = 0; char path[MAX_PATH + 32];
        FILE* f;
        buf[0] = 0;
        _snprintf(path, sizeof(path), "%s\\hub_state.json", g_troot);
        f = fopen(path, "rb");
        if (f) { rd = fread(buf, 1, sizeof(buf) - 1, f); buf[rd] = 0; fclose(f); }
        (void)rd;
        CHECK(strstr(buf, "\"autoMount\":false") != NULL,
              "R5 automount(0) persisted to hub_state.json");
    }
    registry_scan(g_troot);
    state_load(g_troot);
    reconcile_target_refresh();
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(g_plugins[idx_of("p-alpha")].state == PS_REGISTERED,
          "R5 alpha stays unmounted across restart after switch-off");

    /* R6: M3b b3 - TOPO seam filled; converged set stays quiescent under it */
    CHECK(reconcile_run(RECONCILE_ORDER_TOPO, 1, &r) == 0 && r.changed == 0,
          "R6 TOPO order accepted in M3b and quiescent on converged set");

    /* R7: rescan carries session target */
    reconcile_set_target(idx_of("p-beta"), 1);
    registry_rescan(g_troot);
    ib = idx_of("p-beta");
    CHECK(ib >= 0 && g_plugins[ib].want_known == 1 && g_plugins[ib].want_mount == 1,
          "R7 rescan carries want (target survives disk re-read)");
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(g_plugins[ib].state == PS_ACTIVE, "R7 beta mounts per carried target");

    /* R8: corrupt state file recovery keeps convergence */
    {
        char path[MAX_PATH + 32];
        _snprintf(path, sizeof(path), "%s\\hub_state.json", g_troot);
        write_file(path, "{\"version\":1, BROKEN");
    }
    registry_scan(g_troot);
    CHECK(state_load(g_troot) == 0, "R8 corrupt state -> load returns 0 (E5)");
    reconcile_target_refresh();
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(g_plugins[idx_of("p-delta")].state == PS_ACTIVE &&
          g_plugins[idx_of("p-alpha")].state == PS_ACTIVE,
          "R8 after corrupt recovery: manifest defaults drive target (alpha+delta autoMount=true both ACTIVE)");

    /* R9: failing plugin converges to ERROR, never ACTIVE, retried but bounded */
    reconcile_set_target(idx_of("p-fail"), 1);
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(g_plugins[idx_of("p-fail")].state == PS_ERROR && r.failed >= 1,
          "R9 p-fail want=1 -> ERROR (retryable), not ACTIVE");
    reconcile_run(RECONCILE_ORDER_FLAT, 1, &r);
    CHECK(r.changed == 0, "R9 quiescent despite repeated p-fail retries (changed==0)");
}

/* ---------------------------------------------------------------- storm */

static unsigned xs32(unsigned* s)
{
    *s ^= *s << 13; *s ^= *s >> 17; *s ^= *s << 5;
    return *s;
}

/* op weights: 0 SET_TARGET x3, 1 SET_AUTOMOUNT, 2 RECONCILE x3, 3 RESCAN,
 * 4 RESTART x2, 5 MOUNT_DIRECT, 6 UNMOUNT_DIRECT, 7 CORRUPT_LOAD, 8 SAVE,
 * 9 DRAIN_UNMOUNT (b3), 10 VERSION_FLIP (b3 P1) */
static const int g_optab[16] = { 0,0,0, 1, 2,2,2, 3, 4,4, 5, 6, 7, 8, 9, 10 };

typedef struct { int op; char id[64]; int arg; } OpRec;

static void capture_view(int* want, int* active, int* nrec)
{
    int k;
    *nrec = g_n_plugins;
    for (k = 0; k < NPKGS; k++) {
        int i = idx_of(g_ids[k]);
        want[k] = 0; active[k] = 0;
        if (i < 0) continue;
        want[k]   = g_plugins[i].want_known ? g_plugins[i].want_mount
                                            : (g_plugins[i].auto_mount ||
                                               (g_plugins[i].prev_state[0] &&
                                                strcmp(g_plugins[i].prev_state, "ACTIVE") == 0));
        active[k] = (g_plugins[i].state == PS_ACTIVE) ? 1 : 0;
    }
}

static int has_transient(void)
{
    int i;
    for (i = 0; i < g_n_plugins; i++)
        if (g_plugins[i].state == PS_MOUNTING || g_plugins[i].state == PS_UNMOUNTING)
            return 1;
    return 0;
}

static int handle_consistent(void)
{
    int i;
    for (i = 0; i < g_n_plugins; i++) {
        if (g_plugins[i].state == PS_ACTIVE && !g_plugins[i].handle) return 0;
        if (g_plugins[i].state != PS_ACTIVE && g_plugins[i].handle) return 0;
    }
    return 1;
}

static void print_ops(OpRec* h, int n)
{
    int k;
    printf("  op history (%d):\n", n);
    for (k = 0; k < n; k++)
        printf("    [%02d] op=%d id=%s arg=%d\n", k, h[k].op, h[k].id, h[k].arg);
}

static void storm(void)
{
    int seed, seq_fail = 0;
    printf("\n--- O10 confluence storm: %d randomized sequences ---\n", NSTORM);
    g_hub_log_quiet = 1;
    for (seed = 1; seed <= NSTORM; seed++) {
        unsigned s = (unsigned)seed * 2654435761u + 12345u;
        OpRec hist[MAXOPS];
        int nop, k, passes;
        ReconcileResult r;
        int cap_want[NPKGS], cap_active[NPKGS], exp_want[NPKGS], exp_active[NPKGS];
        int nrec = 0, mismatch = 0, bad = 0;
        char capstr[256], expstr[256];

        if (!s) s = 1;
        remove_state_file();
        registry_scan(g_troot);

        nop = 8 + (int)(xs32(&s) % 17);
        if (nop > MAXOPS) nop = MAXOPS;
        for (k = 0; k < nop; k++) {
            int op = g_optab[xs32(&s) % 16];
            int idx, tries = 0;
            hist[k].op = op; hist[k].arg = 0; hist[k].id[0] = 0;
            do {
                idx = (g_n_plugins > 0) ? (int)(xs32(&s) % (unsigned)g_n_plugins) : 0;
            } while (g_n_plugins > 0 && !g_plugins[idx].id[0] && ++tries < 8);
            if (g_n_plugins > 0 && g_plugins[idx].id[0])
                strncpy(hist[k].id, g_plugins[idx].id, sizeof(hist[k].id) - 1);
            switch (op) {
            case 0: hist[k].arg = (int)(xs32(&s) % 2); reconcile_set_target(idx, hist[k].arg); break;
            case 1: hist[k].arg = (int)(xs32(&s) % 2); registry_set_automount(idx, hist[k].arg); break;
            case 2: reconcile_run(RECONCILE_ORDER_TOPO, 1, NULL); break;
            case 3: registry_rescan(g_troot); break;
            case 4: registry_scan(g_troot); state_load(g_troot); reconcile_target_refresh(); break;
            case 5: registry_mount(idx, 1); break;
            case 6: registry_unmount(idx, 1); break;
            case 9: registry_unmount_drain(idx, 1); break;
            case 10: {
                int i_a = idx_of("p-alpha");
                if (i_a >= 0) {
                    char err[64];
                    vm_set_current(g_plugins[i_a].dir,
                                   (xs32(&s) % 2) ? "1.1.0" : "1.0.0", err, sizeof(err));
                    registry_rescan(g_troot);
                }
                break;
            }
            case 7: {
                char path[MAX_PATH + 32];
                _snprintf(path, sizeof(path), "%s\\hub_state.json", g_troot);
                write_file(path, "{\"version\":1, BROKEN");
                state_load(g_troot);
                break;
            }
            case 8: state_save(g_troot); break;
            }
        }
        /* quiesce */
        state_save(g_troot);
        passes = 0;
        do {
            reconcile_run(RECONCILE_ORDER_TOPO, 1, &r);
            passes++;
        } while (r.changed && passes < 3);
        if (r.changed != 0) bad = 1;
        if (has_transient()) bad = 2;
        capture_view(cap_want, cap_active, &nrec);
        if (!handle_consistent()) bad = 3;

        /* static assembly from the final target view alone */
        registry_scan(g_troot);
        for (k = 0; k < NPKGS; k++) {
            int i = idx_of(g_ids[k]);
            if (i >= 0) reconcile_set_target(i, cap_want[k]);
        }
        reconcile_run(RECONCILE_ORDER_TOPO, 1, &r);
        capture_view(exp_want, exp_active, &nrec);
        if (has_transient()) bad = 4;
        if (!handle_consistent()) bad = 5;
        for (k = 0; k < NPKGS; k++)
            if (cap_active[k] != exp_active[k]) mismatch = 1;

        capstr[0] = expstr[0] = 0;
        for (k = 0; k < NPKGS; k++) {
            if (cap_active[k]) { strcat(capstr, g_ids[k]); strcat(capstr, " "); }
            if (exp_active[k]) { strcat(expstr, g_ids[k]); strcat(expstr, " "); }
        }
        if (bad || mismatch) {
            seq_fail++;
            g_hub_log_quiet = 0;
            printf("[STORM-FAIL] seed=%d bad=%d mismatch=%d\n", seed, bad, mismatch);
            printf("  quiescent ACTIVE: %s\n", capstr);
            printf("  static   ACTIVE: %s\n", expstr);
            for (k = 0; k < NPKGS; k++) {
                int i2 = idx_of(g_ids[k]);
                printf("  rec %-8s cap_want=%d exp_want=%d cap_act=%d exp_act=%d "
                       "state=%s wn=%d wm=%d wait=%s nreq=%d req0=%d\n",
                       g_ids[k], cap_want[k], exp_want[k], cap_active[k], exp_active[k],
                       i2 >= 0 ? registry_state_name(g_plugins[i2].state) : "-",
                       i2 >= 0 ? g_plugins[i2].want_known : -1,
                       i2 >= 0 ? g_plugins[i2].want_mount : -1,
                       i2 >= 0 ? g_plugins[i2].wait_reason : "-",
                       i2 >= 0 ? g_plugins[i2].n_requires : -1,
                       (i2 >= 0 && g_plugins[i2].n_requires > 0) ? g_plugins[i2].req_idx[0] : -9);
            }
            print_ops(hist, nop);
            g_hub_log_quiet = 1;
        }
        if (seed % 100 == 0) {
            g_hub_log_quiet = 0;
            printf("  storm progress: %d/%d (failures so far: %d)\n", seed, NSTORM, seq_fail);
            g_hub_log_quiet = 1;
        }
    }
    g_hub_log_quiet = 0;
    CHECK(seq_fail == 0, "O10 storm: 1000 random sequences, quiescent == static assembly");
    printf("storm done: %d/%d sequences confluent\n", NSTORM - seq_fail, NSTORM);
}

int main(void)
{
    printf("=== creo-hub M2 reconcile OFFLINE test (G3/R4 + O10 storm) ===\n\n");
    setup_root();
    fixture_checks();
    storm();
    remove_state_file();
    printf("\n=== RESULT: %d pass, %d fail -> %s ===\n",
           g_pass, g_fail, g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
