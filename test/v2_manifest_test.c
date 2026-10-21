/* =====================================================================
 * creo-hub M2 OFFLINE test: plugin.json v2 lenient read (docs\13 G2) +
 * R1 mandatory structural validation (docs\15) + R2 version-aware
 * fingerprint. Gate O11 / DoD 15. Hermetic temp root, no Creo, no PTK.
 * v1 byte-identical behavior is additionally guarded by registry_test
 * (O6, 39/39) which runs against the real M1 fixture set.
 * Exit code = number of failures (0 = ALL PASS).
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hub_log.h"
#include "registry.h"
#include "state.h"

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

static char g_troot[MAX_PATH];
static char g_mbuf[16384];

/* ------------------------------------------------------------ fs helpers */

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

static void setup_root(void)
{
    char sub[MAX_PATH], st[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, sub);
    sub[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_v2test", sub);
    mkdir_p(g_troot);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\probe\\bin", g_troot);        mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\probe\\text\\text\\usascii", g_troot); mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\plugins\\probe\\bin\\probe.dll", g_troot);
    write_file(sub, "MZ-fake-dll-for-offline-test");
    _snprintf(sub, sizeof(sub), "%s\\plugins\\probe\\text\\text\\usascii\\probe_messages.txt", g_troot);
    write_file(sub, "#MSG dummy\n");
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
}

static void put_probe_manifest(const char* json)
{
    char path[MAX_PATH + 64];
    _snprintf(path, sizeof(path), "%s\\plugins\\probe\\plugin.json", g_troot);
    write_file(path, json);
}

/* v2 wrapper: inner = extra members starting with ',' (commands/requires/...) */
static const char* J2(const char* inner)
{
    _snprintf(g_mbuf, sizeof(g_mbuf),
        "{\"manifestVersion\":2,\"id\":\"probe\",\"name\":\"probe pkg\",\"version\":\"1.0.0\","
        "\"adapter\":\"ptk-sync\",\"entry\":\"bin/probe.dll\",\"textDir\":\"text\"%s}",
        inner);
    return g_mbuf;
}

/* ------------------------------------------------------------- fixtures */

#define GOOD_CMDS \
    ",\"commands\":[" \
    "{\"id\":\"probe.open\",\"title\":\"Open widget\",\"desc\":\"Opens the probe widget dialog\"," \
    "\"descForLLM\":\"Open the probe widget; takes no arguments\",\"risk\":\"safe\"," \
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}}," \
    "{\"id\":\"probe.set\",\"title\":\"Set value\",\"desc\":\"Sets the probe numeric value\"," \
    "\"descForLLM\":\"Set the probe value; requires numeric 'value'\",\"risk\":\"confirm\"," \
    "\"inputSchema\":{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"number\",\"description\":\"target value\"}},\"required\":[\"value\"]}," \
    "\"timeoutSec\":5,\"reversible\":true,\"idempotent\":false}]" \
    ",\"requires\":[\"core.ui\"],\"provides\":[\"probe.widget\"]"

typedef struct {
    const char* name;
    const char* json;      /* full manifest text */
    int expect_valid;      /* 1 REGISTERED, 0 INVALID */
    const char* reason_has;/* INVALID reason must contain this */
} Case;

int main(void)
{
    Case cases[64];
    int nc = 0, ci, iprobe;
    unsigned long long fp_before;

    printf("=== creo-hub M2 v2-manifest OFFLINE test (G2/R1/R2, gate O11) ===\n\n");
    setup_root();
    strcpy(g_root, g_troot);

    /* ---- case table: good ---- */
    cases[nc].name = "good-full-v2";   cases[nc].json = J2(GOOD_CMDS); cases[nc].expect_valid = 1; cases[nc].reason_has = NULL; nc++;
    /* NOTE: J2 uses one shared buffer; copy each json into strdup-ed storage */
    {
        char* store0 = _strdup(cases[0].json);
        cases[0].json = store0;

        cases[nc].name = "good-min-cmd";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.go\",\"title\":\"Go\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"destructive\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 1; cases[nc].reason_has = NULL; nc++;

        cases[nc].name = "good-no-commands";
        cases[nc].json = _strdup(J2(""));
        cases[nc].expect_valid = 1; cases[nc].reason_has = NULL; nc++;

        cases[nc].name = "good-schema-items-1level";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.list\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"tags\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}}]}"));
        cases[nc].expect_valid = 1; cases[nc].reason_has = NULL; nc++;

        /* ---- case table: bad manifestVersion ---- */
        cases[nc].name = "bad-mv-3";
        cases[nc].json = _strdup(J2("")); 
        {   /* patch manifestVersion to 3 */
            char* s = (char*)cases[nc].json;
            char* p = strstr(s, "\"manifestVersion\":2");
            if (p) p[18] = '3';
        }
        cases[nc].expect_valid = 0; cases[nc].reason_has = "manifestVersion"; nc++;

        cases[nc].name = "bad-mv-string";
        cases[nc].json = _strdup("{\"manifestVersion\":\"2\",\"id\":\"probe\",\"adapter\":\"ptk-sync\",\"entry\":\"bin/probe.dll\",\"textDir\":\"text\"}");
        cases[nc].expect_valid = 0; cases[nc].reason_has = "manifestVersion"; nc++;

        /* ---- case table: bad commands shape ---- */
        cases[nc].name = "bad-commands-not-array";
        cases[nc].json = _strdup(J2(",\"commands\":{\"id\":\"probe.x\"}"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "commands must be an array"; nc++;

        cases[nc].name = "bad-cmd-not-object";
        cases[nc].json = _strdup(J2(",\"commands\":[\"probe.x\"]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "must be an object"; nc++;

        cases[nc].name = "bad-cmd-id-namespace";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"other.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "must start with"; nc++;

        /* ---- required per-command fields ---- */
        cases[nc].name = "bad-title-missing";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "title"; nc++;

        cases[nc].name = "bad-title-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":5,\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "title"; nc++;

        cases[nc].name = "bad-desc-missing";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "desc"; nc++;

        cases[nc].name = "bad-descForLLM-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":{},\"risk\":\"safe\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "descForLLM"; nc++;

        cases[nc].name = "bad-risk-missing";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "risk"; nc++;

        cases[nc].name = "bad-risk-value";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"dangerous\",\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "risk"; nc++;

        cases[nc].name = "bad-risk-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":2,\"inputSchema\":{}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "risk"; nc++;

        cases[nc].name = "bad-inputSchema-missing";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\"}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "inputSchema"; nc++;

        cases[nc].name = "bad-inputSchema-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":\"none\"}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "inputSchema"; nc++;

        /* ---- inputSchema subset ---- */
        cases[nc].name = "bad-schema-ref";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{\"$ref\":\"#/other\"}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "unsupported keyword"; nc++;

        cases[nc].name = "bad-schema-proptype";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"c\":{\"type\":\"color\"}}}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "type"; nc++;

        cases[nc].name = "bad-schema-toplevel-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "top level"; nc++;

        cases[nc].name = "bad-schema-deep-items";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"array\",\"items\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}}}}}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "nested deeper"; nc++;

        cases[nc].name = "bad-schema-required-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{\"type\":\"object\",\"required\":\"value\"}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "required"; nc++;

        cases[nc].name = "bad-schema-addlprop-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{\"type\":\"object\",\"additionalProperties\":\"no\"}}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "additionalProperties"; nc++;

        /* ---- optional-but-typed fields ---- */
        cases[nc].name = "bad-timeout-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{},\"timeoutSec\":\"10\"}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "timeoutSec"; nc++;

        cases[nc].name = "bad-timeout-negative";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{},\"timeoutSec\":-5}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "timeoutSec"; nc++;

        cases[nc].name = "bad-reversible-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{},\"reversible\":\"yes\"}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "reversible"; nc++;

        cases[nc].name = "bad-idempotent-type";
        cases[nc].json = _strdup(J2(",\"commands\":[{\"id\":\"probe.x\",\"title\":\"t\",\"desc\":\"d\",\"descForLLM\":\"l\",\"risk\":\"safe\",\"inputSchema\":{},\"idempotent\":1}]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "idempotent"; nc++;

        /* ---- requires/provides shape ---- */
        cases[nc].name = "bad-requires-not-array";
        cases[nc].json = _strdup(J2(",\"requires\":\"core.ui\""));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "requires"; nc++;

        cases[nc].name = "bad-requires-elem";
        cases[nc].json = _strdup(J2(",\"requires\":[1,2]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "requires[0]"; nc++;

        cases[nc].name = "bad-requires-too-many";
        cases[nc].json = _strdup(J2(",\"requires\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "max 8"; nc++;

        cases[nc].name = "bad-provides-not-array";
        cases[nc].json = _strdup(J2(",\"provides\":{\"x\":1}"));
        cases[nc].expect_valid = 0; cases[nc].reason_has = "provides"; nc++;

        /* ---- v1 regression (byte-identical behavior under lenient read) ---- */
        cases[nc].name = "v1-regression";
        cases[nc].json = _strdup("{\"manifestVersion\":1,\"id\":\"probe\",\"name\":\"probe v1\",\"version\":\"0.9.0\","
                                 "\"adapter\":\"ptk-sync\",\"entry\":\"bin/probe.dll\",\"textDir\":\"text\",\"autoMount\":false,"
                                 "\"commands\":[{\"id\":\"probe.legacy\",\"risk\":\"nonsense\",\"whatever\":42}],"
                                 "\"requires\":\"ignored-in-v1\"}");
        cases[nc].expect_valid = 1; cases[nc].reason_has = NULL; nc++;

        /* ---- run the matrix ---- */
        printf("--- O11 fixture matrix: %d cases ---\n", nc);
        for (ci = 0; ci < nc; ci++) {
            char label[160];
            put_probe_manifest(cases[ci].json);
            registry_scan(g_troot);
            iprobe = registry_find_idx("probe");
            _snprintf(label, sizeof(label), "O11 %s -> %s", cases[ci].name,
                      cases[ci].expect_valid ? "REGISTERED" : "INVALID");
            if (iprobe < 0) { CHECK(0, label); continue; }
            if (cases[ci].expect_valid) {
                CHECK(g_plugins[iprobe].state == PS_REGISTERED, label);
                if (g_plugins[iprobe].state != PS_REGISTERED)
                    printf("       reason was: %s\n", g_plugins[iprobe].invalid_reason);
            } else {
                int ok = (g_plugins[iprobe].state == PS_INVALID) &&
                         g_plugins[iprobe].invalid_reason[0] &&
                         (!cases[ci].reason_has ||
                          strstr(g_plugins[iprobe].invalid_reason, cases[ci].reason_has) != NULL);
                CHECK(ok, label);
                if (!ok)
                    printf("       state=%s reason='%s' (wanted substring '%s')\n",
                           registry_state_name(g_plugins[iprobe].state),
                           g_plugins[iprobe].invalid_reason,
                           cases[ci].reason_has ? cases[ci].reason_has : "(any)");
            }
        }

        /* ---- G2 passthrough detail on good-full ---- */
        put_probe_manifest(store0);
        registry_scan(g_troot);
        iprobe = registry_find_idx("probe");
        CHECK(iprobe >= 0 && g_plugins[iprobe].manifest_version == 2, "G2 good-full manifest_version==2");
        CHECK(iprobe >= 0 && g_plugins[iprobe].n_commands == 2, "G2 good-full 2 commands parsed");
        if (iprobe >= 0) {
            PluginCmd* c0 = &g_plugins[iprobe].cmdv[0];
            PluginCmd* c1 = &g_plugins[iprobe].cmdv[1];
            CHECK(strcmp(c0->title, "Open widget") == 0 && strcmp(c0->risk, "safe") == 0,
                  "G2 cmd0 title+risk passthrough");
            CHECK(strcmp(c0->desc_llm, "Open the probe widget; takes no arguments") == 0,
                  "G2 cmd0 descForLLM passthrough");
            CHECK(strstr(c0->input_schema, "additionalProperties") != NULL,
                  "G2 cmd0 inputSchema raw JSON passthrough");
            CHECK(c0->timeout_sec == 0 && c0->reversible == -1 && c0->idempotent == -1,
                  "G2 cmd0 optional fields default (0/-1/-1)");
            CHECK(c1->timeout_sec == 5 && c1->reversible == 1 && c1->idempotent == 0 &&
                  strcmp(c1->risk, "confirm") == 0,
                  "G2 cmd1 timeoutSec/reversible/idempotent/risk passthrough");
            CHECK(strstr(c1->input_schema, "\"required\":[\"value\"]") != NULL,
                  "G2 cmd1 schema keeps required list");
            CHECK(g_plugins[iprobe].n_requires == 1 &&
                  strcmp(g_plugins[iprobe].requires_list[0], "core.ui") == 0,
                  "G2 requires parsed (not enforced in M2)");
            CHECK(g_plugins[iprobe].n_provides == 1 &&
                  strcmp(g_plugins[iprobe].provides_list[0], "probe.widget") == 0,
                  "G2 provides parsed (P3 exposure)");
        }

        /* ---- v1 regression detail ---- */
        put_probe_manifest(cases[nc - 1].json);
        registry_scan(g_troot);
        iprobe = registry_find_idx("probe");
        CHECK(iprobe >= 0 && g_plugins[iprobe].manifest_version == 1 &&
              g_plugins[iprobe].n_commands == 1 &&
              strcmp(g_plugins[iprobe].commands[0], "probe.legacy") == 0,
              "DoD11 v1 parses identically (id-only command list)");
        CHECK(iprobe >= 0 && g_plugins[iprobe].cmdv[0].title[0] == 0 &&
              g_plugins[iprobe].n_requires == 0,
              "DoD11 v1 ignores risk/unknown/requires fields (byte-identical behavior)");

        /* ---- R2 fingerprint ---- */
        printf("--- R2 fingerprint ---\n");
        put_probe_manifest("{\"manifestVersion\":1,\"id\":\"probe\",\"adapter\":\"ptk-sync\","
                           "\"entry\":\"bin/probe.dll\",\"textDir\":\"text\",\"version\":\"1.0.0\"}");
        registry_scan(g_troot);
        iprobe = registry_find_idx("probe");
        fp_before = g_plugins[iprobe].fingerprint;
        CHECK(fp_before != 0, "R2 fingerprint computed (nonzero)");
        registry_rescan(g_troot);
        iprobe = registry_find_idx("probe");
        CHECK(g_plugins[iprobe].fingerprint == fp_before &&
              g_plugins[iprobe].fingerprint_changed == 0,
              "R2 unchanged manifest -> stable fingerprint, no badge");
        /* mount, then edit manifest on disk -> rescan -> badge, still ACTIVE */
        registry_mount(iprobe, 1);
        CHECK(g_plugins[iprobe].state == PS_ACTIVE, "R2 probe mounted (stub)");
        put_probe_manifest("{\"manifestVersion\":1,\"id\":\"probe\",\"adapter\":\"ptk-sync\","
                           "\"entry\":\"bin/probe.dll\",\"textDir\":\"text\",\"version\":\"1.0.1\"}");
        registry_rescan(g_troot);
        iprobe = registry_find_idx("probe");
        CHECK(g_plugins[iprobe].fingerprint != fp_before, "R2 edited manifest -> new fingerprint");
        CHECK(g_plugins[iprobe].fingerprint_changed == 1, "R2 ACTIVE + changed -> badge flag set");
        CHECK(g_plugins[iprobe].state == PS_ACTIVE && g_plugins[iprobe].handle != NULL,
              "R2 NO auto-remount: still ACTIVE with live handle (human decision)");
        /* remount clears badge */
        registry_unmount(iprobe, 1);
        registry_mount(iprobe, 1);
        CHECK(g_plugins[iprobe].state == PS_ACTIVE && g_plugins[iprobe].fingerprint_changed == 0,
              "R2 fresh mount clears badge");

        /* ---- b4 D5: manifest_lint <-> registry.c shared-fixture contract.
         * The SAME tree (test\lint_fixtures) + SAME golden verdicts
         * (test\lint_expected.txt) that manifest_lint.mjs is pinned to.
         * Two independent implementations, one frozen expectation: any
         * parse-rule drift on either side fails its own gate (docs\23 §6.4,
         * risk-12 institutionalized). ---- */
        printf("--- D5 lint-pair (shared fixtures + golden verdicts) ---\n");
        {
            char xcmd[2 * MAX_PATH + 64];
            FILE* gf;
            int pairs = 0, mismatches = 0;
            _snprintf(xcmd, sizeof(xcmd),
                      "xcopy test\\lint_fixtures\\* \"%s\\plugins\" /E /I /Y >nul 2>&1",
                      g_troot);
            CHECK(system(xcmd) == 0, "D5 fixtures copied into scan root");
            registry_scan(g_troot);
            gf = fopen("test\\lint_expected.txt", "r");
            CHECK(gf != NULL, "D5 golden verdict file readable");
            if (gf) {
                char line[512];
                while (fgets(line, sizeof(line), gf)) {
                    char fdir[128], verdict[32], kw[256], fpath[MAX_PATH + 160];
                    int idx = -1, k2;
                    char* nl;
                    nl = strchr(line, '\n'); if (nl) *nl = '\0';
                    nl = strchr(line, '\r'); if (nl) *nl = '\0';
                    if (!line[0] || line[0] == '#') continue;
                    kw[0] = '-'; kw[1] = '\0';
                    if (sscanf(line, "%127s %31s %255[^\r\n]", fdir, verdict, kw) < 2) continue;
                    pairs++;
                    /* record->dir holds the FULL package path (field finding:
                     * the scan log prints it, the registry keeps it absolute) */
                    _snprintf(fpath, sizeof(fpath), "%s\\plugins\\%s", g_troot, fdir);
                    for (k2 = 0; k2 < g_n_plugins; k2++)
                        if (_stricmp(g_plugins[k2].dir, fpath) == 0) { idx = k2; break; }
                    if (idx < 0) {
                        printf("[FAIL] D5 %s: not scanned\n", fdir);
                        mismatches++; g_fail++;
                        continue;
                    }
                    if (strcmp(verdict, "OK") == 0) {
                        if (g_plugins[idx].state == PS_REGISTERED) {
                            printf("[PASS] D5 %s OK (REGISTERED)\n", fdir);
                            g_pass++;
                        } else {
                            printf("[FAIL] D5 %s: expected REGISTERED got %s (%s)\n", fdir,
                                   registry_state_name(g_plugins[idx].state),
                                   g_plugins[idx].invalid_reason);
                            mismatches++; g_fail++;
                        }
                    } else if (strcmp(verdict, "UNSUPPORTED") == 0) {
                        if (g_plugins[idx].state == PS_UNSUPPORTED &&
                            (kw[0] == '-' || strstr(g_plugins[idx].invalid_reason, kw))) {
                            printf("[PASS] D5 %s UNSUPPORTED\n", fdir);
                            g_pass++;
                        } else {
                            printf("[FAIL] D5 %s: expected UNSUPPORTED/'%s' got %s (%s)\n", fdir, kw,
                                   registry_state_name(g_plugins[idx].state),
                                   g_plugins[idx].invalid_reason);
                            mismatches++; g_fail++;
                        }
                    } else { /* INVALID */
                        if (g_plugins[idx].state == PS_INVALID &&
                            (kw[0] == '-' || strstr(g_plugins[idx].invalid_reason, kw))) {
                            printf("[PASS] D5 %s INVALID ('%s')\n", fdir, kw);
                            g_pass++;
                        } else {
                            printf("[FAIL] D5 %s: expected INVALID/'%s' got %s (%s)\n", fdir, kw,
                                   registry_state_name(g_plugins[idx].state),
                                   g_plugins[idx].invalid_reason);
                            mismatches++; g_fail++;
                        }
                    }
                }
                fclose(gf);
            }
            CHECK(pairs == 23 && mismatches == 0,
                  "D5 all 23 fixture verdicts agree with the golden file (lint pair)");
        }

        for (ci = 0; ci < nc; ci++) free((void*)cases[ci].json);
        {
            char st[MAX_PATH + 32];
            _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
            remove(st);
        }
        printf("\n=== RESULT: %d pass, %d fail -> %s ===\n",
               g_pass, g_fail, g_fail ? "FAIL" : "ALL PASS");
        return g_fail ? 1 : 0;
    }
}
