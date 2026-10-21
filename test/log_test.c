/* =====================================================================
 * creo-hub M3b b4 OFFLINE test: R1 hub_log rotation (docs\25 b4).
 * Links the REAL hub_log.c (production rotation code). PTK UI/array calls
 * are stubbed locally (hub_popup is never invoked here).
 *   - primary log grows past cap -> renamed to hub_m1.log.1, fresh log
 *   - single generation: second rotation REPLACES .1
 *   - cap <=0 disables rotation
 * Exit code = number of failures (0 = ALL PASS).
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <ProToolkit.h>
#include <ProArray.h>
#include <ProUIMessage.h>

#include "hub_log.h"

/* --- local PTK stubs (only symbols hub_log.c imports) --- */
ProError ProArrayAlloc(int n, int elem_size, int incr, ProArray* arr)
{ (void)n; (void)elem_size; (void)incr; *arr = NULL; return PRO_TK_NO_ERROR; }
ProError ProArrayFree(ProArray* arr) { (void)arr; return PRO_TK_NO_ERROR; }
ProError ProUIMessageDialogDisplay(ProUIMessageType type, wchar_t* title,
                                   wchar_t* msg_txt, ProUIMessageButton* buttons,
                                   ProUIMessageButton def_button,
                                   ProUIMessageButton* user_choice)
{
    (void)type; (void)title; (void)msg_txt; (void)buttons;
    *user_choice = def_button;
    return PRO_TK_NO_ERROR;
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

static char g_troot[MAX_PATH];

static long file_size(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    return (long)fad.nFileSizeLow;
}

int main(void)
{
    char logdir[MAX_PATH], logpath[MAX_PATH], rotpath[MAX_PATH], cmd[MAX_PATH + 32];
    char tmp[MAX_PATH];
    DWORD n;
    int i;

    n = GetTempPathA(MAX_PATH, tmp);
    tmp[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_log", tmp);
    _snprintf(cmd, sizeof(cmd), "cmd /c rd /s /q \"%s\" >nul 2>&1", g_troot);
    system(cmd);
    _snprintf(logdir, sizeof(logdir), "%s\\logs", g_troot);
    CreateDirectoryA(g_troot, NULL);
    CreateDirectoryA(logdir, NULL);
    _snprintf(logpath, sizeof(logpath), "%s\\hub_m1.log", logdir);
    _snprintf(rotpath, sizeof(rotpath), "%s\\hub_m1.log.1", logdir);

    strcpy(g_root, g_troot);
    g_hub_log_max_bytes = 2048;

    printf("=== creo-hub M3b b4 log rotation OFFLINE test ===\n\n");

    printf("--- L1 rotate at cap, single generation ---\n");
    for (i = 0; i < 60; i++)
        hub_log("TEST", "rotation seed line %02d padding-padding-padding", i);
    CHECK(file_size(rotpath) >= 2048, "L1 .1 exists and holds >= cap bytes");
    CHECK(file_size(logpath) > 0 && file_size(logpath) < 2048 + 256,
          "L1 fresh primary log restarted (small)");
    {   /* first rotation content = EARLY lines; fresh log = LATE lines */
        FILE* f = fopen(rotpath, "rb");
        char head[512] = "";
        if (f) { fread(head, 1, sizeof(head) - 1, f); fclose(f); }
        CHECK(strstr(head, "seed line 00") != NULL, "L1 .1 starts with oldest lines");
        f = fopen(logpath, "rb");
        head[0] = '\0';
        if (f) {
            char buf[8192];
            size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
            buf[rd] = '\0';
            fclose(f);
            CHECK(strstr(buf, "seed line 59") != NULL, "L1 newest line in primary log");
        } else {
            CHECK(0, "L1 newest line in primary log");
        }
        (void)head;
    }
    {   /* second rotation replaces .1 (single generation, no .2 ever) */
        char rot2[MAX_PATH + 8];
        _snprintf(rot2, sizeof(rot2), "%s\\hub_m1.log.2", logdir);
        for (i = 60; i < 140; i++)
            hub_log("TEST", "rotation second-wave line %02d padding-padding", i);
        CHECK(file_size(rot2) == -1, "L1 no .2 generation ever appears");
        CHECK(file_size(rotpath) >= 2048, "L1 .1 replaced by newer rotation");
    }

    printf("--- L2 cap disabled -> no rotation ---\n");
    DeleteFileA(rotpath);
    DeleteFileA(logpath);
    g_hub_log_max_bytes = 0;
    for (i = 0; i < 60; i++)
        hub_log("TEST", "unrotated line %02d padding-padding-padding-padding", i);
    CHECK(file_size(rotpath) == -1, "L2 cap<=0: .1 never created");
    CHECK(file_size(logpath) > 2048, "L2 primary log grows past old cap");

    printf("\n=== log_test: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail;
}
