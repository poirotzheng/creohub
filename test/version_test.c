/* =====================================================================
 * creo-hub M3b b3 OFFLINE test: P1 version manager (docs\14 5.4 / docs\23 3.6)
 *   - thin root pointer plugin.json {"currentVersion":"<v>"} redirects the
 *     effective manifest to versions\<v>\plugin.json (immutable dirs)
 *   - classic layout (no pointer) keeps working = zero migration
 *   - vm_list/vm_has/vm_set_current (atomic .tmp + replace)
 *   - pointer switch + rescan flips commands/entry/fingerprint
 *   - missing version dir / id mismatch -> INVALID with readable reason
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
#include "version_mgr.h"

extern int g_hub_log_quiet;

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

static char g_troot[MAX_PATH];

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

static void mkver(const char* id, const char* ver, const char* cmdid)
{
    char d[MAX_PATH], f[MAX_PATH], body[1024];
    _snprintf(d, sizeof(d), "%s\\plugins\\%s\\versions\\%s", g_troot, id, ver);
    _snprintf(f, sizeof(f), "%s\\bin", d);                      mkdir_p(f);
    _snprintf(f, sizeof(f), "%s\\text\\text\\usascii", d);      mkdir_p(f);
    _snprintf(f, sizeof(f), "%s\\bin\\%s.dll", d, id);
    write_file(f, "MZ-fake-dll-for-offline-test");
    _snprintf(f, sizeof(f), "%s\\text\\text\\usascii\\%s_messages.txt", d, id);
    write_file(f, "#MSG dummy\n");
    _snprintf(f, sizeof(f), "%s\\plugin.json", d);
    _snprintf(body, sizeof(body),
        "{\"manifestVersion\":2,\"id\":\"%s\",\"name\":\"%s pkg\",\"version\":\"%s\","
        "\"adapter\":\"ptk-sync\",\"entry\":\"bin/%s.dll\",\"textDir\":\"text\","
        "\"commands\":[{\"id\":\"%s\",\"title\":\"t\",\"desc\":\"d\","
        "\"descForLLM\":\"d\",\"risk\":\"safe\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}]",
        id, id, ver, id, cmdid);
    write_file(f, body);
}

static void setup_root(void)
{
    char d[MAX_PATH], f[MAX_PATH], cmd[MAX_PATH + 32], st[MAX_PATH + 32];
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    tmp[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_version", tmp);
    _snprintf(cmd, sizeof(cmd), "cmd /c rd /s /q \"%s\" >nul 2>&1", g_troot);
    system(cmd);
    mkdir_p(g_troot);

    /* v-ptr: thin pointer + two immutable version dirs */
    _snprintf(d, sizeof(d), "%s\\plugins\\v-ptr", g_troot);    mkdir_p(d);
    _snprintf(f, sizeof(f), "%s\\plugin.json", d);
    write_file(f,
        "{\"manifestVersion\":2,\"id\":\"v-ptr\",\"adapter\":\"ptk-sync\","
        "\"currentVersion\":\"1.0.0\"}");
    mkver("v-ptr", "1.0.0", "v-ptr.one");
    mkver("v-ptr", "1.1.0", "v-ptr.eleven");

    /* v-old: classic layout regression (no pointer) */
    _snprintf(d, sizeof(d), "%s\\plugins\\v-old\\bin", g_troot);            mkdir_p(d);
    _snprintf(d, sizeof(d), "%s\\plugins\\v-old\\text\\text\\usascii", g_troot); mkdir_p(d);
    _snprintf(d, sizeof(d), "%s\\plugins\\v-old\\bin\\v-old.dll", g_troot);
    write_file(d, "MZ-fake");
    _snprintf(d, sizeof(d), "%s\\plugins\\v-old\\text\\text\\usascii\\v-old_messages.txt", g_troot);
    write_file(d, "#MSG dummy\n");
    _snprintf(d, sizeof(d), "%s\\plugins\\v-old\\plugin.json", g_troot);
    write_file(d,
        "{\"manifestVersion\":2,\"id\":\"v-old\",\"name\":\"old\",\"version\":\"1.0.0\","
        "\"adapter\":\"ptk-sync\",\"entry\":\"bin/v-old.dll\",\"textDir\":\"text\","
        "\"commands\":[{\"id\":\"v-old.one\",\"title\":\"t\",\"desc\":\"d\","
        "\"descForLLM\":\"d\",\"risk\":\"safe\","
        "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}}]}");

    /* v-badptr: pointer to a missing version dir */
    _snprintf(d, sizeof(d), "%s\\plugins\\v-badptr", g_troot);  mkdir_p(d);
    _snprintf(f, sizeof(f), "%s\\plugin.json", d);
    write_file(f,
        "{\"manifestVersion\":2,\"id\":\"v-badptr\",\"adapter\":\"ptk-sync\","
        "\"currentVersion\":\"9.9.9\"}");

    /* v-mism: version manifest id disagrees with the pointer id */
    _snprintf(d, sizeof(d), "%s\\plugins\\v-mism", g_troot);    mkdir_p(d);
    _snprintf(f, sizeof(f), "%s\\plugin.json", d);
    write_file(f,
        "{\"manifestVersion\":2,\"id\":\"v-mism\",\"adapter\":\"ptk-sync\","
        "\"currentVersion\":\"1.0.0\"}");
    _snprintf(d, sizeof(d), "%s\\plugins\\v-mism\\versions\\1.0.0\\bin", g_troot); mkdir_p(d);
    _snprintf(d, sizeof(d), "%s\\plugins\\v-mism\\versions\\1.0.0\\text\\text\\usascii", g_troot); mkdir_p(d);
    _snprintf(d, sizeof(d), "%s\\plugins\\v-mism\\versions\\1.0.0\\bin\\x.dll", g_troot);
    write_file(d, "MZ-fake");
    _snprintf(d, sizeof(d), "%s\\plugins\\v-mism\\versions\\1.0.0\\text\\text\\usascii\\x_messages.txt", g_troot);
    write_file(d, "#MSG dummy\n");
    _snprintf(d, sizeof(d), "%s\\plugins\\v-mism\\versions\\1.0.0\\plugin.json", g_troot);
    write_file(d,
        "{\"manifestVersion\":2,\"id\":\"other-id\",\"adapter\":\"ptk-sync\","
        "\"entry\":\"bin/x.dll\",\"textDir\":\"text\"}");

    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);      remove(st);
    _snprintf(st, sizeof(st), "%s\\hub_state.json.tmp", g_troot);  remove(st);
}

int main(void)
{
    int ip, io, ib, im, i;
    unsigned long long fp10, fp11;
    char vers[8][32], pkg[MAX_PATH], err[128];

    g_hub_log_quiet = 1;
    GetCurrentDirectoryA(MAX_PATH, g_root);
    setup_root();
    strcpy(g_root, g_troot);
    printf("=== creo-hub M3b b3 version_mgr OFFLINE test ===\n\n");

    printf("--- V1 pointer redirection + classic regression ---\n");
    registry_scan(g_troot);
    CHECK(g_n_plugins == 4, "V1 scan finds 4 packages");
    ip = registry_find_idx("v-ptr"); io = registry_find_idx("v-old");
    ib = registry_find_idx("v-badptr"); im = registry_find_idx("v-mism");
    CHECK(ip >= 0 && g_plugins[ip].state == PS_REGISTERED &&
          strcmp(g_plugins[ip].current_version, "1.0.0") == 0,
          "V1 pointer package REGISTERED with currentVersion 1.0.0");
    CHECK(ip >= 0 && g_plugins[ip].n_commands == 1 &&
          strcmp(g_plugins[ip].commands[0], "v-ptr.one") == 0,
          "V1 effective commands come from versions\\1.0.0 manifest");
    CHECK(ip >= 0 && strstr(g_plugins[ip].entry_abs, "versions\\1.0.0\\bin\\v-ptr.dll"),
          "V1 entry_abs resolved under the version dir");
    CHECK(io >= 0 && g_plugins[io].state == PS_REGISTERED &&
          g_plugins[io].current_version[0] == '\0' &&
          strstr(g_plugins[io].entry_abs, "plugins\\v-old\\bin\\v-old.dll") != NULL,
          "V1 classic layout unchanged (zero migration)");
    CHECK(ib >= 0 && g_plugins[ib].state == PS_INVALID &&
          strstr(g_plugins[ib].invalid_reason, "versions") != NULL,
          "V1 missing version dir -> INVALID with readable reason");
    CHECK(im >= 0 && g_plugins[im].state == PS_INVALID &&
          strstr(g_plugins[im].invalid_reason, "id") != NULL,
          "V1 version manifest id mismatch -> INVALID");

    printf("--- V2 vm_list / vm_has ---\n");
    _snprintf(pkg, sizeof(pkg), "%s\\plugins\\v-ptr", g_troot);
    i = vm_list(pkg, vers, 8);
    CHECK(i == 2 && strcmp(vers[0], "1.0.0") == 0 && strcmp(vers[1], "1.1.0") == 0,
          "V2 vm_list returns sorted version dirs with manifests");
    CHECK(vm_has(pkg, "1.1.0") == 1 && vm_has(pkg, "9.9.9") == 0 &&
          vm_has(pkg, "..\\..") == 0,
          "V2 vm_has true/false + path-ish rejected");

    printf("--- V3 pointer switch + rescan flips effective manifest ---\n");
    fp10 = g_plugins[ip].fingerprint;
    err[0] = '\0';
    CHECK(vm_set_current(pkg, "1.1.0", err, sizeof(err)) == 1, "V3 vm_set_current 1.1.0 ok");
    registry_rescan(g_troot);
    ip = registry_find_idx("v-ptr");
    CHECK(ip >= 0 && strcmp(g_plugins[ip].current_version, "1.1.0") == 0 &&
          g_plugins[ip].n_commands == 1 &&
          strcmp(g_plugins[ip].commands[0], "v-ptr.eleven") == 0,
          "V3 rescan picks up 1.1.0 commands");
    CHECK(ip >= 0 && strstr(g_plugins[ip].entry_abs, "versions\\1.1.0\\bin\\v-ptr.dll"),
          "V3 entry_abs follows the pointer");
    fp11 = g_plugins[ip].fingerprint;
    CHECK(fp10 != fp11, "V3 fingerprint changes with the version (double insurance)");

    printf("--- V4 rollback pointer (P1b manual) ---\n");
    CHECK(vm_set_current(pkg, "1.0.0", err, sizeof(err)) == 1, "V4 vm_set_current back to 1.0.0");
    registry_rescan(g_troot);
    ip = registry_find_idx("v-ptr");
    CHECK(ip >= 0 && strcmp(g_plugins[ip].commands[0], "v-ptr.one") == 0 &&
          g_plugins[ip].fingerprint == fp10,
          "V4 rollback restores 1.0.0 manifest + fingerprint");

    printf("\n=== version_test: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail;
}
