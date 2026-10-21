/* =====================================================================
 * creo-hub M1 offline test: registry scan/validate + state machine +
 * hub_state.json persistence, all WITHOUT Creo (stub adapter/log/popup).
 * Exit code = number of failures (0 = ALL PASS).
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "hub_log.h"
#include "registry.h"
#include "state.h"
#include "ledger.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

static void cleanup_state_files(void)
{
    char path[MAX_PATH];
    sprintf(path, "%s\\hub_state.json", g_root);
    remove(path);
    sprintf(path, "%s\\hub_state.json.tmp", g_root);
    remove(path);
}

/* Optional external sample package: present in the private tree, absent in the
 * public snapshot. Tests that need it are skipped, not failed. */
static const char *state_file(void)
{
    static char path[MAX_PATH];
    sprintf(path, "%s\\hub_state.json", g_root);
    return path;
}

static int has_package(const char *id)
{
    char path[MAX_PATH];
    sprintf(path, "plugins\\%s", id);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

int main(void)
{
    int i_demo, i_demo2, i_demofail, i_bad1, i_demo3, i_demoslot, i_exampleplugin;
    int ok = 0, fail = 0, att;
    char expected_entry[MAX_PATH];

    GetCurrentDirectoryA(sizeof(g_root) - 1, g_root);
    cleanup_state_files();

    printf("=== creo-hub M1 registry/state OFFLINE test ===\n\n");

    /* ---- T1: scan + validation ---- */
    registry_scan(g_root);
    CHECK(g_n_plugins == 6 + has_package("exampleplugin"),
          "T1 scan finds expected packages (6 + optional external sample)");
    i_demo     = registry_find_idx("demo");
    i_demo2    = registry_find_idx("demo2");
    i_demofail = registry_find_idx("demofail");
    i_bad1     = registry_find_idx("bad1");
    i_demo3    = registry_find_idx("demo3");
    i_demoslot = registry_find_idx("demoslot");
    i_exampleplugin   = registry_find_idx("exampleplugin");
    CHECK(i_demo >= 0 && g_plugins[i_demo].state == PS_REGISTERED, "T1 demo REGISTERED");
    CHECK(i_demo >= 0 && g_plugins[i_demo].auto_mount == 1, "T1 demo autoMount=1 (manifest)");
    sprintf(expected_entry, "%s\\plugins\\demo\\bin\\demo.dll", g_root);
    CHECK(i_demo >= 0 && strcmp(g_plugins[i_demo].entry_abs, expected_entry) == 0,
          "T1 demo entry_abs normalized");
    CHECK(i_demo >= 0 && g_plugins[i_demo].n_commands == 1 &&
          strcmp(g_plugins[i_demo].commands[0], "demo.hello") == 0, "T1 demo command parsed + namespace ok");
    CHECK(i_demo2 >= 0 && g_plugins[i_demo2].state == PS_REGISTERED &&
          g_plugins[i_demo2].auto_mount == 0, "T1 demo2 REGISTERED autoMount=0");
    CHECK(i_demofail >= 0 && g_plugins[i_demofail].state == PS_REGISTERED, "T1 demofail REGISTERED");
    CHECK(i_bad1 >= 0 && g_plugins[i_bad1].state == PS_INVALID, "T1 bad1 INVALID");
    CHECK(i_bad1 >= 0 && strstr(g_plugins[i_bad1].invalid_reason, "id") != NULL,
          "T1 bad1 reason mentions missing id");

    /* ---- T1c: optional external sample pointer package (exampleplugin) ----
     * Thin pointer at the package root redirects to versions\<currentVersion>\
     * plugin.json; commands/version must come from the version manifest, not
     * the pointer. The live pointer moves as the user ships versions, so
     * assert semantics (redirected + valid) rather than a frozen value.
     * Present in the private tree, absent in the public snapshot -> skip. */
    if (has_package("exampleplugin")) {
        CHECK(i_exampleplugin >= 0 && g_plugins[i_exampleplugin].state == PS_REGISTERED,
              "T1 sample REGISTERED (pointer package)");
        CHECK(i_exampleplugin >= 0 && g_plugins[i_exampleplugin].manifest_version == 2,
              "T1 sample manifest v2 (version manifest)");
        CHECK(i_exampleplugin >= 0 && g_plugins[i_exampleplugin].current_version[0] != '\0',
              "T1 sample currentVersion resolved (pointer redirection)");
        CHECK(i_exampleplugin >= 0 && g_plugins[i_exampleplugin].n_commands == 10,
              "T1 sample 10 commands from version manifest");
    } else {
        printf("[SKIP] optional external sample package not present\n");
    }

    /* ---- T1b: M3a packages (demo3 = v2 command-face carrier, demoslot = E12) ---- */
    CHECK(i_demo3 >= 0 && g_plugins[i_demo3].state == PS_REGISTERED &&
          g_plugins[i_demo3].auto_mount == 0, "T1 demo3 REGISTERED autoMount=0 (E11 carrier)");
    CHECK(i_demo3 >= 0 && g_plugins[i_demo3].manifest_version == 2, "T1 demo3 manifest v2");
    CHECK(i_demo3 >= 0 && g_plugins[i_demo3].n_commands == 5, "T1 demo3 has 5 commands (M4b +longtask)");
    CHECK(i_demo3 >= 0 &&
          strcmp(g_plugins[i_demo3].commands[0], "demo3.echo") == 0 &&
          strcmp(g_plugins[i_demo3].commands[1], "demo3.slow") == 0 &&
          strcmp(g_plugins[i_demo3].commands[2], "demo3.fail") == 0 &&
          strcmp(g_plugins[i_demo3].commands[3], "demo3.boom") == 0,
          "T1 demo3 command ids namespaced, manifest order");
    CHECK(i_demo3 >= 0 &&
          strcmp(g_plugins[i_demo3].cmdv[0].risk, "safe") == 0 &&
          strcmp(g_plugins[i_demo3].cmdv[2].risk, "confirm") == 0 &&
          strcmp(g_plugins[i_demo3].cmdv[3].risk, "destructive") == 0 &&
          g_plugins[i_demo3].cmdv[3].reversible == 0,
          "T1 demo3 risk ladder + boom reversible=0 parsed");
    CHECK(i_demo3 >= 0 && g_plugins[i_demo3].cmdv[1].timeout_sec == 2,
          "T1 demo3.slow timeoutSec=2 parsed (E11-8)");
    CHECK(i_demo3 >= 0 && strstr(g_plugins[i_demo3].cmdv[0].desc_llm, "echo") != NULL,
          "T1 demo3 descForLLM parsed");
    CHECK(i_demo3 >= 0 && strstr(g_plugins[i_demo3].cmdv[0].input_schema, "msg") != NULL,
          "T1 demo3 inputSchema serialized (M2 subset accepted)");
    CHECK(i_demoslot >= 0 && g_plugins[i_demoslot].state == PS_REGISTERED &&
          g_plugins[i_demoslot].auto_mount == 0 &&
          g_plugins[i_demoslot].n_commands == 0,
          "T1 demoslot REGISTERED autoMount=0 no commands (E12)");

    /* ---- T2: persistence save ---- */
    CHECK(state_save(g_root) == 1, "T2 state_save returns 1");
    {
        FILE* f = fopen(state_file(), "rb");
        CHECK(f != NULL, "T2 hub_state.json created");
        if (f) fclose(f);
    }

    /* ---- T3: load applies overrides ---- */
    {
        FILE* f = fopen(state_file(), "wb");
        fputs("{\"version\":1,\"updatedAt\":\"test\",\"plugins\":{"
              "\"demo\":{\"autoMount\":false,\"lastState\":\"ACTIVE\"},"
              "\"demo2\":{\"autoMount\":true,\"lastState\":\"REGISTERED\"}}}", f);
        fclose(f);
        registry_scan(g_root);                 /* reset to manifest defaults */
        CHECK(state_load(g_root) == 1, "T3 state_load returns 1");
        i_demo  = registry_find_idx("demo");
        i_demo2 = registry_find_idx("demo2");
        CHECK(i_demo >= 0 && g_plugins[i_demo].auto_mount == 0, "T3 demo autoMount overridden to 0");
        CHECK(i_demo2 >= 0 && g_plugins[i_demo2].auto_mount == 1, "T3 demo2 autoMount overridden to 1");
        CHECK(i_demo >= 0 && strcmp(g_plugins[i_demo].prev_state, "ACTIVE") == 0, "T3 demo prev_state=ACTIVE");
    }

    /* ---- T4: corrupt state file tolerance (E5) ---- */
    {
        FILE* f = fopen(state_file(), "wb");
        fputs("{\"version\":1, BROKEN", f);
        fclose(f);
        registry_scan(g_root);
        CHECK(state_load(g_root) == 0, "T4 corrupt state -> returns 0, no crash (E5)");
        i_demo = registry_find_idx("demo");
        CHECK(i_demo >= 0 && g_plugins[i_demo].auto_mount == 1, "T4 manifest defaults restored after corrupt");
    }

    /* ---- T5: state machine guards + transitions (stub adapter) ---- */
    cleanup_state_files();
    registry_scan(g_root);
    i_demo     = registry_find_idx("demo");
    i_demo2    = registry_find_idx("demo2");
    i_demofail = registry_find_idx("demofail");
    i_bad1     = registry_find_idx("bad1");

    CHECK(registry_mount(i_demo, 1) == PRO_TK_NO_ERROR, "T5 mount demo -> NO_ERROR");
    CHECK(g_plugins[i_demo].state == PS_ACTIVE, "T5 demo ACTIVE after mount");
    CHECK(registry_mount(i_demo, 1) == PRO_TK_E_IN_USE, "T5 remount ACTIVE refused E_IN_USE");
    CHECK(g_plugins[i_demo].state == PS_ACTIVE, "T5 still ACTIVE after refused remount");
    CHECK(registry_unmount(i_demo2, 1) != PRO_TK_NO_ERROR, "T5 unmount not-mounted refused");
    CHECK(registry_unmount(i_demo, 1) == PRO_TK_NO_ERROR, "T5 unmount demo -> NO_ERROR");
    CHECK(g_plugins[i_demo].state == PS_REGISTERED, "T5 demo REGISTERED after unmount");
    CHECK(g_plugins[i_demo].handle == NULL, "T5 handle cleared after unmount");

    CHECK(registry_mount(i_demofail, 1) == PRO_TK_GENERAL_ERROR, "T5 mount demofail -> GENERAL_ERROR (stub, M0-measured)");
    CHECK(g_plugins[i_demofail].state == PS_ERROR, "T5 demofail ERROR (retryable, not TAINTED)");
    CHECK(registry_mount(i_demofail, 1) == PRO_TK_GENERAL_ERROR, "T5 demofail retry from ERROR allowed");
    CHECK(registry_mount(i_bad1, 1) == PRO_TK_BAD_CONTEXT, "T5 mount bad1 refused (INVALID)");

    /* ---- T6: automount_all selects only autoMount=1 ---- */
    cleanup_state_files();
    registry_scan(g_root);
    att = registry_automount_all(1, &ok, &fail);
    CHECK(att == 1 && ok == 1 && fail == 0, "T6 automount_all attempts exactly 1 (demo), ok");
    CHECK(g_plugins[registry_find_idx("demo")].state == PS_ACTIVE, "T6 demo ACTIVE after automount");

    /* ---- T7: rescan carries live state ---- */
    registry_rescan(g_root);
    CHECK(g_plugins[registry_find_idx("demo")].state == PS_ACTIVE, "T7 rescan carries ACTIVE");
    CHECK(g_plugins[registry_find_idx("demo")].handle != NULL, "T7 rescan carries handle");
    CHECK(g_n_plugins == 7, "T7 rescan still 7 packages");

    /* ---- T8: S10 fix - lastState persistence + prev-ACTIVE restore ---- */
    cleanup_state_files();
    {
        FILE* f = fopen(state_file(), "wb");
        fputs("{\"version\":1,\"updatedAt\":\"t\",\"plugins\":{"
              "\"demo2\":{\"autoMount\":false,\"lastState\":\"ACTIVE\"}}}", f);
        fclose(f);
        registry_scan(g_root);
        CHECK(state_load(g_root) == 1, "T8 state with demo2 lastState=ACTIVE loaded");
        /* lifecycle save must NOT downgrade demo2's persisted ACTIVE */
        state_save(g_root);
        {
            char buf[2048]; size_t rd = 0; buf[0] = 0;
            f = fopen(state_file(), "rb");
            if (f) { rd = fread(buf, 1, sizeof(buf) - 1, f); buf[rd] = 0; fclose(f); }
            CHECK(strstr(buf, "\"demo2\":{\"autoMount\":false,\"lastState\":\"ACTIVE\"}") != NULL,
                  "T8 lifecycle save keeps demo2 lastState=ACTIVE (no downgrade)");
        }
        /* restore set = autoMount(demo) + prevACTIVE(demo2) */
        ok = 0; fail = 0;
        att = registry_automount_all(1, &ok, &fail);
        CHECK(att == 2 && ok == 2 && fail == 0, "T8 auto-init attempts demo(autoMount)+demo2(prevACTIVE)");
        CHECK(g_plugins[registry_find_idx("demo2")].state == PS_ACTIVE, "T8 demo2 restored ACTIVE");
        /* explicit unmount must clear the restore mark on disk */
        registry_unmount(registry_find_idx("demo2"), 1);
        {
            char buf[2048]; size_t rd = 0; buf[0] = 0;
            f = fopen(state_file(), "rb");
            if (f) { rd = fread(buf, 1, sizeof(buf) - 1, f); buf[rd] = 0; fclose(f); }
            CHECK(strstr(buf, "\"demo2\":{\"autoMount\":false,\"lastState\":\"REGISTERED\"}") != NULL,
                  "T8 explicit unmount persists lastState=REGISTERED");
        }
    }

    /* ---- T9 (M3b b3 R8/E12): refused unload -> TAINTED + P2 ledger drained ---- */
    {
        extern int g_stub_unmount_fail;
        int i_d2 = registry_find_idx("demo2");
        CHECK(i_d2 >= 0 && registry_mount(i_d2, 1) == PRO_TK_NO_ERROR &&
              g_plugins[i_d2].state == PS_ACTIVE, "T9 demo2 mounted for E12");
        CHECK(i_d2 >= 0 && ledger_count(i_d2) >= 1,
              "T9 P2 ledger holds the mount anchor while ACTIVE");
        g_stub_unmount_fail = 1;
        CHECK(i_d2 >= 0 && registry_unmount(i_d2, 1) != PRO_TK_NO_ERROR,
              "T9 refused unload returns error");
        g_stub_unmount_fail = 0;
        CHECK(i_d2 >= 0 && g_plugins[i_d2].state == PS_TAINTED,
              "T9 E12: unload failure -> TAINTED (restart Creo recommended)");
        CHECK(i_d2 >= 0 && ledger_count(i_d2) == 0,
              "T9 P2 ledger fully drained despite TAINTED");
        /* test cleanup: TAINTED refuses unmount, reset the record by hand */
        if (i_d2 >= 0) {
            g_plugins[i_d2].handle = NULL;
            g_plugins[i_d2].state = PS_REGISTERED;
            strncpy(g_plugins[i_d2].prev_state, "REGISTERED",
                    sizeof(g_plugins[i_d2].prev_state) - 1);
        }
    }

    cleanup_state_files();
    printf("\n=== RESULT: %d pass, %d fail -> %s ===\n",
           g_pass, g_fail, g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
