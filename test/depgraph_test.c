/* =====================================================================
 * creo-hub M3b b3 OFFLINE test: dependency graph (docs\22 8.1 / docs\23 3.5)
 *   - requires resolution (plugin id first, then provides-service)
 *   - scan-time cycle detection: members INVALID + "dependency cycle: a->b->a"
 *   - missing dep != failure: REGISTERED + "waiting for: <id>" badge
 *   - reconcile TOPO: providers before consumers (Kahn), waiting stays put
 *   - drain-unmount: transitive dependents reverse-topo, target last
 *   - provider arrival: rescan + reconcile auto-activates the waiter
 * Hermetic synthetic tree under %TEMP%; no Creo, no PTK libs.
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

extern int g_hub_log_quiet;
extern char g_stub_mount_order[32][64];
extern int  g_stub_mount_n;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

static char g_troot[MAX_PATH];

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
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\') { *p = '\0'; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    }
    CreateDirectoryA(tmp, NULL);
}

/* requires_csv / provides_csv may be "" */
static void mkpkg(const char* root, const char* id,
                  const char* requires_csv, const char* provides_csv)
{
    char dir[MAX_PATH], sub[MAX_PATH], json[MAX_PATH], body[1024];
    _snprintf(dir, sizeof(dir), "%s\\plugins\\%s", root, id);
    _snprintf(sub, sizeof(sub), "%s\\bin", dir);            mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\text\\text\\usascii", dir); mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\bin\\%s.dll", dir, id);
    write_file(sub, "MZ-fake-dll-for-offline-test");
    _snprintf(sub, sizeof(sub), "%s\\text\\text\\usascii\\%s_messages.txt", dir, id);
    write_file(sub, "#MSG dummy\n");
    _snprintf(json, sizeof(json), "%s\\plugin.json", dir);
    _snprintf(body, sizeof(body),
        "{\"manifestVersion\":2,\"id\":\"%s\",\"name\":\"%s pkg\",\"version\":\"1.0.0\","
        "\"adapter\":\"ptk-sync\",\"entry\":\"bin/%s.dll\",\"textDir\":\"text\","
        "\"requires\":[%s],\"provides\":[%s],"
        "\"commands\":[{\"id\":\"%s.one\",\"title\":\"one\",\"desc\":\"t\","
        "\"descForLLM\":\"t\",\"risk\":\"safe\",\"inputSchema\":{\"type\":\"object\"}}]}",
        id, id, id, requires_csv, provides_csv, id);
    write_file(json, body);
}

static void setup_root(void)
{
    char st[MAX_PATH + 32];
    char cmd[MAX_PATH + 32];
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    tmp[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_depgraph", tmp);
    _snprintf(cmd, sizeof(cmd), "cmd /c rd /s /q \"%s\" >nul 2>&1", g_troot);
    system(cmd);                       /* stale tree from a previous run would skew counts */
    mkdir_p(g_troot);
    mkpkg(g_troot, "g-base", "",            "\"svc.base\"");
    mkpkg(g_troot, "g-mid",  "\"g-base\"",  "");
    mkpkg(g_troot, "g-top",  "\"g-mid\"",   "");
    mkpkg(g_troot, "g-svc",  "\"svc.base\"", "");
    mkpkg(g_troot, "g-cyc1", "\"g-cyc2\"",  "");
    mkpkg(g_troot, "g-cyc2", "\"g-cyc1\"",  "");
    mkpkg(g_troot, "g-wait", "\"g-ghost\"", "");
    mkpkg(g_troot, "g-lone", "",            "");
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);      remove(st);
    _snprintf(st, sizeof(st), "%s\\hub_state.json.tmp", g_troot);  remove(st);
}

static int idx_of(const char* id) { return registry_find_idx(id); }

static int pos_of(const char* id)
{
    int i;
    for (i = 0; i < g_stub_mount_n; i++)
        if (strcmp(g_stub_mount_order[i], id) == 0) return i;
    return -1;
}

int main(void)
{
    ReconcileResult r;
    int ibase, imid, itop, isvc, ic1, ic2, iwait, ilone;
    int order[MAX_PLUGINS], n, k, pw, pm;

    g_hub_log_quiet = 1;
    GetCurrentDirectoryA(MAX_PATH, g_root);   /* state file target unused (hermetic root) */
    setup_root();
    strcpy(g_root, g_troot);
    printf("=== creo-hub M3b b3 depgraph OFFLINE test ===\n\n");

    /* ---- D1: scan-time resolution + cycle + waiting badge ---- */
    printf("--- D1 scan: resolve / cycle / waiting ---\n");
    registry_scan(g_troot);
    CHECK(g_n_plugins == 8, "D1 scan finds 8 synthetic packages");
    ibase = idx_of("g-base"); imid = idx_of("g-mid"); itop = idx_of("g-top");
    isvc  = idx_of("g-svc");  ic1  = idx_of("g-cyc1"); ic2 = idx_of("g-cyc2");
    iwait = idx_of("g-wait"); ilone = idx_of("g-lone");
    CHECK(imid >= 0 && g_plugins[imid].n_requires == 1 &&
          g_plugins[imid].req_idx[0] == ibase,
          "D1 g-mid requires resolved to g-base index");
    CHECK(isvc >= 0 && g_plugins[isvc].req_idx[0] == ibase,
          "D1 g-svc requires resolved via provides-service svc.base");
    CHECK(ic1 >= 0 && g_plugins[ic1].state == PS_INVALID &&
          g_plugins[ic2].state == PS_INVALID,
          "D1 cycle members both INVALID at scan time");
    CHECK(ic1 >= 0 && strstr(g_plugins[ic1].invalid_reason, "dependency cycle:") &&
          strstr(g_plugins[ic1].invalid_reason, "g-cyc1") &&
          strstr(g_plugins[ic1].invalid_reason, "g-cyc2"),
          "D1 cycle reason carries readable chain");
    CHECK(iwait >= 0 && g_plugins[iwait].state == PS_REGISTERED &&
          strcmp(g_plugins[iwait].wait_reason, "waiting for: g-ghost") == 0,
          "D1 missing dep: REGISTERED + waiting badge (not a failure)");
    CHECK(ilone >= 0 && g_plugins[ilone].wait_reason[0] == '\0' &&
          g_plugins[ilone].req_idx[0] == -1,
          "D1 independent record: no badge, empty req slot = -1");

    /* ---- D2: reconcile TOPO mounts providers first ---- */
    printf("--- D2 reconcile TOPO order ---\n");
    g_stub_mount_n = 0;
    for (k = 0; k < g_n_plugins; k++) reconcile_set_target(k, 1);
    CHECK(reconcile_run(RECONCILE_ORDER_TOPO, 1, &r) == 0, "D2 TOPO run returns 0");
    CHECK(g_plugins[ibase].state == PS_ACTIVE && g_plugins[imid].state == PS_ACTIVE &&
          g_plugins[itop].state == PS_ACTIVE && g_plugins[isvc].state == PS_ACTIVE &&
          g_plugins[ilone].state == PS_ACTIVE,
          "D2 all satisfiable records ACTIVE after TOPO run");
    CHECK(g_plugins[iwait].state == PS_REGISTERED,
          "D2 waiter stays REGISTERED (dep missing, never forced)");
    CHECK(g_plugins[ic1].state == PS_INVALID && r.skipped >= 2,
          "D2 cycle members skipped (INVALID never forced)");
    CHECK(pos_of("g-base") >= 0 && pos_of("g-mid") > pos_of("g-base") &&
          pos_of("g-top") > pos_of("g-mid") && pos_of("g-svc") > pos_of("g-base"),
          "D2 mount order: base before mid before top; base before svc");

    /* ---- D3: drain plan shape ---- */
    printf("--- D3 drain plan (reverse topo) ---\n");
    n = registry_drain_plan(ibase, order, MAX_PLUGINS);
    CHECK(n == 4, "D3 plan over base closure = {top,mid,svc,base}");
    CHECK(n == 4 && order[n - 1] == ibase, "D3 target last in drain order");
    pw = pm = -1;
    for (k = 0; k < n; k++) {
        if (order[k] == itop) pw = k;
        if (order[k] == imid) pm = k;
    }
    CHECK(pw >= 0 && pm >= 0 && pw < pm, "D3 deepest dependent (top) before mid");

    /* ---- D4: drain unmount cascades dependents, leaves others ---- */
    printf("--- D4 drain unmount ---\n");
    CHECK(registry_unmount_drain(ibase, 1) == PRO_TK_NO_ERROR, "D4 drain unmount ok");
    CHECK(g_plugins[ibase].state == PS_REGISTERED && g_plugins[imid].state == PS_REGISTERED &&
          g_plugins[itop].state == PS_REGISTERED && g_plugins[isvc].state == PS_REGISTERED,
          "D4 whole closure REGISTERED after drain");
    CHECK(g_plugins[ilone].state == PS_ACTIVE, "D4 unrelated record untouched");
    n = registry_drain_plan(itop, order, MAX_PLUGINS);
    CHECK(n == 1 && order[0] == itop, "D4 plan of a leaf = itself only");

    /* ---- D5: provider arrival auto-activates the waiter ---- */
    printf("--- D5 provider arrival ---\n");
    mkpkg(g_troot, "g-ghost", "", "");
    registry_rescan(g_troot);
    iwait = idx_of("g-wait");
    CHECK(iwait >= 0 && g_plugins[iwait].wait_reason[0] == '\0' &&
          g_plugins[iwait].req_idx[0] == idx_of("g-ghost"),
          "D5 rescan resolves the arrived provider, badge cleared");
    reconcile_set_target(idx_of("g-ghost"), 1);
    reconcile_set_target(iwait, 1);
    reconcile_set_target(ibase, 1); reconcile_set_target(imid, 1);
    reconcile_set_target(itop, 1);  reconcile_set_target(isvc, 1);
    reconcile_set_target(ilone, 1);
    CHECK(reconcile_run(RECONCILE_ORDER_TOPO, 1, &r) == 0 && r.mounted >= 2,
          "D5 TOPO run mounts ghost then waiter");
    CHECK(g_plugins[iwait].state == PS_ACTIVE &&
          g_plugins[idx_of("g-ghost")].state == PS_ACTIVE,
          "D5 waiter ACTIVE after provider arrival (timing-only deps invariant)");

    /* ---- D6: leaf unmount does not cascade downwards ---- */
    printf("--- D6 leaf unmount isolation ---\n");
    itop = idx_of("g-top");
    CHECK(registry_unmount_drain(itop, 1) == PRO_TK_NO_ERROR &&
          g_plugins[itop].state == PS_REGISTERED &&
          g_plugins[idx_of("g-mid")].state == PS_ACTIVE,
          "D6 unmounting a consumer leaves its provider ACTIVE");

    printf("\n=== depgraph_test: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail;
}
