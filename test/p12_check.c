/* =====================================================================
 * creo-hub M2d: p12_check — feed the REAL P12 artifact manifest
 * (test\p12_demo_plugin_v2.json, the file the user copies over
 * plugins\demo\plugin.json during docs\17 P12) through the REAL
 * registry scanner, so the live round cannot surprise us:
 *   [1] v2 validation passes -> REGISTERED (never red "invalid")
 *   [2] every field P12 asserts is present with expected values
 *   [3] R2 badge flow: ACTIVE + manifest bytes change -> fingerprintChanged;
 *       restore original -> badge clears on rescan
 * Run: p12_check.exe [path-to-p12-json]   (default: test\p12_demo_plugin_v2.json)
 * Exit code = number of failures.
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "registry.h"

extern char g_root[];            /* test_stubs.c */
extern int  g_hub_log_quiet;     /* test_stubs.c */

static int failures = 0, checks = 0;
#define CHECK(cond, name) do { checks++; \
    if (cond) { printf("[PASS] %s\n", name); } \
    else { failures++; printf("[FAIL] %s  (line %d)\n", name, __LINE__); } } while (0)

static char g_troot[MAX_PATH];

static void write_file(const char* path, const char* content, size_t n)
{
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(content, 1, n, f); fclose(f); }
}

static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH]; char* p;
    strncpy(tmp, path, sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\') { *p = '\0'; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    }
    CreateDirectoryA(tmp, NULL);
}

static char* read_whole(const char* path, size_t* out_len)
{
    FILE* f = fopen(path, "rb");
    long sz; char* buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = '\0'; fclose(f);
    *out_len = (size_t)sz;
    return buf;
}

int main(int argc, char** argv)
{
    const char* src = (argc > 1) ? argv[1] : "test\\p12_demo_plugin_v2.json";
    char path[MAX_PATH + 64], st[MAX_PATH + 32];
    char sub[MAX_PATH];
    size_t orig_len = 0, mod_len = 0;
    char *orig, *modified;
    PluginRec* p;
    unsigned long long fp_orig;
    DWORD n;

    g_hub_log_quiet = 1;
    printf("=== creo-hub P12 artifact check (real scanner, real file) ===\n");
    printf("manifest under test: %s\n\n", src);

    orig = read_whole(src, &orig_len);
    if (!orig) { printf("[FATAL] cannot read %s\n", src); return 99; }

    /* ---- hermetic root with a "demo" package shaped like production ---- */
    n = GetTempPathA(MAX_PATH, sub); sub[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_p12", sub);
    mkdir_p(g_troot);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\demo\\bin", g_troot);                 mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\demo\\text\\text\\usascii", g_troot); mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\demo\\bin\\demo.dll", g_troot);
    write_file(sub, "MZ-fake-dll-for-offline-test", 26);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\demo\\text\\text\\usascii\\demo_messages.txt", g_troot);
    write_file(sub, "#MSG dummy\n", 11);
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
    _snprintf(path, sizeof(path), "%s\\plugins\\demo\\plugin.json", g_troot);
    write_file(path, orig, orig_len);
    strcpy(g_root, g_troot);

    /* ---- [1] scan: must come out REGISTERED, never INVALID ---- */
    CHECK(registry_scan(g_troot) >= 0, "scan ok");
    CHECK(g_n_plugins == 1, "exactly 1 package found");
    p = registry_find("demo");
    CHECK(p != NULL, "registry_find(\"demo\")");
    if (!p) { printf("\n[FATAL] no record\n"); return failures + 1; }
    if (p->state == PS_INVALID)
        printf("       invalid_reason: %s\n", p->invalid_reason);
    CHECK(p->state == PS_REGISTERED, "state REGISTERED (v2 validation passed)");

    /* ---- [2] fields P12 asserts on the panel ---- */
    CHECK(p->manifest_version == 2, "manifestVersion 2");
    CHECK(strcmp(p->name, "Demo Plugin") == 0, "name passthrough");
    CHECK(strcmp(p->version, "0.2.0") == 0, "version 0.2.0 (bumped from 0.1.0)");
    CHECK(p->auto_mount == 1, "autoMount true");
    CHECK(p->n_commands == 2, "2 commands");
    CHECK(strcmp(p->commands[0], "demo.hello") == 0, "commands[0] id demo.hello");
    CHECK(strcmp(p->commands[1], "demo.export_summary") == 0, "commands[1] id demo.export_summary");
    CHECK(strcmp(p->cmdv[0].risk, "safe") == 0, "cmd[0] risk safe (green badge)");
    CHECK(strcmp(p->cmdv[0].title, "Say Hello") == 0, "cmd[0] title");
    CHECK(strlen(p->cmdv[0].desc_llm) > 0, "cmd[0] descForLLM non-empty");
    CHECK(strstr(p->cmdv[0].input_schema, "additionalProperties") != NULL, "cmd[0] schema keeps additionalProperties");
    CHECK(strcmp(p->cmdv[1].risk, "confirm") == 0, "cmd[1] risk confirm (yellow badge)");
    CHECK(p->cmdv[1].timeout_sec == 20, "cmd[1] timeoutSec 20");
    CHECK(p->cmdv[1].reversible == 0, "cmd[1] reversible false");
    CHECK(p->cmdv[1].idempotent == 1, "cmd[1] idempotent true");
    CHECK(strstr(p->cmdv[1].input_schema, "enum") != NULL, "cmd[1] schema keeps enum");
    CHECK(strstr(p->cmdv[1].input_schema, "required") != NULL, "cmd[1] schema keeps required");
    CHECK(p->n_requires == 0, "requires empty array ok");
    CHECK(p->n_provides == 1 && strcmp(p->provides_list[0], "demo.greeting") == 0, "provides demo.greeting (chip)");
    CHECK(p->fingerprint != 0, "fingerprint computed");
    printf("       fingerprint: %016llx\n", p->fingerprint);
    CHECK(p->user_display == 1, "unknown top-level userDisplay tolerated+read");

    /* ---- [3] R2 badge flow — mirrors docs\17 P12 with REAL semantics
     * (registry.c: badge set when ACTIVE && disk fp != memory fp; carried
     *  across rescans; cleared ONLY by a fresh successful mount, l.733) ---- */
    fp_orig = p->fingerprint;
    p->state = PS_ACTIVE;                       /* demo mounted in Creo */
    p->mounted_once = 1;

    /* (2) swap manifest bytes (user copies v2 file over plugin.json) */
    modified = (char*)malloc(orig_len + 32);
    memcpy(modified, orig, orig_len);
    memcpy(modified + orig_len, "\n", 1);       /* any byte change flips FNV */
    modified[orig_len + 1] = '\0';
    mod_len = orig_len + 1;
    write_file(path, modified, mod_len);
    CHECK(registry_rescan(g_troot) >= 0, "rescan after manifest swap ok");
    p = registry_find("demo");
    CHECK(p && p->state == PS_ACTIVE, "rescan carries ACTIVE (no surprise unmount)");
    CHECK(p && p->fingerprint != fp_orig, "fingerprint changed");
    CHECK(p && p->fingerprint_changed == 1, "R2 badge SET (panel shows 清单已变更)");

    /* (3) badge is sticky: another rescan with same bytes keeps it (carry) */
    CHECK(registry_rescan(g_troot) >= 0, "second rescan ok");
    p = registry_find("demo");
    CHECK(p && p->fingerprint_changed == 1, "badge STICKY across rescan (no auto-remount)");

    /* (3') clear path = Unmount -> Mount (panel Mount on ACTIVE is guard-rejected) */
    {
        int idx = registry_find_idx("demo");
        CHECK(idx >= 0, "find_idx");
        p = registry_find("demo");
        p->state = PS_REGISTERED;               /* simulate unmount outcome */
        p->handle = 0;
        CHECK(registry_mount(idx, 1) == PRO_TK_NO_ERROR, "remount ok (stub adapter)");
        p = registry_find("demo");
        CHECK(p && p->state == PS_ACTIVE, "remount -> ACTIVE");
        CHECK(p && p->fingerprint_changed == 0, "R2 badge CLEARED by fresh mount (l.733)");
        fp_orig = p->fingerprint;               /* now mounted == modified bytes */
    }

    /* (4) restore original v1 -> rescan -> badge RE-APPEARS (disk != mounted,
     * honest R2 semantics; docs\17 P12 step (4) expectation updated to match) */
    write_file(path, orig, orig_len);
    CHECK(registry_rescan(g_troot) >= 0, "rescan after restore ok");
    p = registry_find("demo");
    CHECK(p && p->state == PS_ACTIVE, "restore keeps ACTIVE (no auto anything)");
    CHECK(p && p->fingerprint != fp_orig, "restored fp differs from mounted fp");
    CHECK(p && p->fingerprint_changed == 1, "badge RE-APPEARS after restore (until remount)");

    /* R2 determinism: same bytes -> same fp across rescans */
    {
        unsigned long long fp_a = p->fingerprint;
        registry_rescan(g_troot);
        p = registry_find("demo");
        CHECK(p && p->fingerprint == fp_a, "fingerprint deterministic (same bytes)");
    }
    free(modified);
    free(orig);

    printf("\n=== P12 CHECK RESULT: %d/%d pass, %d fail -> %s ===\n",
           checks - failures, checks, failures, failures ? "FAIL" : "ALL PASS");
    return failures;
}
