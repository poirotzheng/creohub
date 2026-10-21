#include "version_mgr.h"
#include "hub_log.h"
#include "vendor/cjson/cJSON.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "VMGR"

static char* vm_read_whole_file(const char* path)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD sz, got;
    char* buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    sz = GetFileSize(h, NULL);
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf || !ReadFile(h, buf, sz, &got, NULL)) {
        free(buf);
        CloseHandle(h);
        return NULL;
    }
    buf[got] = '\0';
    CloseHandle(h);
    return buf;
}

int vm_valid_ver_chars(const char* v)
{
    if (!v || !v[0] || strlen(v) > 31) return 0;
    for (; *v; v++) {
        if (!(*v >= '0' && *v <= '9') && !(*v >= 'a' && *v <= 'z') &&
            !(*v >= 'A' && *v <= 'Z') && *v != '.' && *v != '-' && *v != '+')
            return 0;
    }
    return 1;
}

int vm_list(const char* pkg_dir, char out[][32], int max)
{
    char pattern[MAX_PATH + 64];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int n = 0, i, j;
    if (!pkg_dir || max < 1) return 0;
    _snprintf(pattern, sizeof(pattern), "%s\\versions\\*", pkg_dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char jp[MAX_PATH + 64];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (!vm_valid_ver_chars(fd.cFileName)) continue;
        _snprintf(jp, sizeof(jp), "%s\\versions\\%s\\plugin.json", pkg_dir, fd.cFileName);
        if (GetFileAttributesA(jp) == INVALID_FILE_ATTRIBUTES) continue;
        if (n < max) {
            strncpy(out[n], fd.cFileName, 31);
            out[n][31] = '\0';
            n++;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (i = 1; i < n; i++) {          /* insertion sort, ascending */
        char tmp[32];
        strcpy(tmp, out[i]);
        for (j = i - 1; j >= 0 && strcmp(out[j], tmp) > 0; j--)
            strcpy(out[j + 1], out[j]);
        strcpy(out[j + 1], tmp);
    }
    return n;
}

int vm_has(const char* pkg_dir, const char* ver)
{
    char jp[MAX_PATH + 64];
    if (!vm_valid_ver_chars(ver)) return 0;
    _snprintf(jp, sizeof(jp), "%s\\versions\\%s\\plugin.json", pkg_dir, ver);
    return GetFileAttributesA(jp) != INVALID_FILE_ATTRIBUTES;
}

int vm_set_current(const char* pkg_dir, const char* ver, char* err, int errsz)
{
    char jp[MAX_PATH + 64], tmp[MAX_PATH + 64];
    char* txt;
    cJSON* root;
    cJSON* it;
    char* out;
    int ok = 0;
    if (!vm_valid_ver_chars(ver)) {
        if (err) _snprintf(err, errsz, "bad version string");
        return 0;
    }
    _snprintf(jp, sizeof(jp), "%s\\plugin.json", pkg_dir);
    txt = vm_read_whole_file(jp);
    if (!txt) {
        if (err) _snprintf(err, errsz, "root plugin.json not readable");
        return 0;
    }
    root = cJSON_Parse(txt);
    free(txt);
    if (!root) {
        if (err) _snprintf(err, errsz, "root plugin.json parse error");
        return 0;
    }
    it = cJSON_GetObjectItemCaseSensitive(root, "currentVersion");
    if (it) cJSON_ReplaceItemInObjectCaseSensitive(root, "currentVersion",
                                                   cJSON_CreateString(ver));
    else    cJSON_AddStringToObject(root, "currentVersion", ver);
    out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!out) {
        if (err) _snprintf(err, errsz, "serialize failed");
        return 0;
    }
    _snprintf(tmp, sizeof(tmp), "%s.tmp", jp);
    {
        FILE* f = fopen(tmp, "wb");
        if (f) {
            fputs(out, f);
            fclose(f);
            ok = 1;
        }
    }
    cJSON_free(out);
    if (!ok) {
        if (err) _snprintf(err, errsz, "tmp write failed");
        return 0;
    }
    if (!MoveFileExA(tmp, jp, MOVEFILE_REPLACE_EXISTING)) {
        if (err) _snprintf(err, errsz, "atomic replace failed (%lu)",
                           (unsigned long)GetLastError());
        DeleteFileA(tmp);
        return 0;
    }
    hub_log(TAG, "pointer switch: %s -> currentVersion=%s", pkg_dir, ver);
    return 1;
}
