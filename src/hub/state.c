/* creo-hub M1: hub_state.json persistence (load/save, corrupt-tolerant - E5) */
#include "state.h"
#include "registry.h"
#include "hub_log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define TAG "PERSIST"

static char* st_read_whole_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    long len;
    char* buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

int state_load(const char* root)
{
    char path[MAX_PATH + 32];
    char* txt;
    cJSON* jroot;
    cJSON* jplugins;
    int i;

    _snprintf(path, sizeof(path), "%s\\hub_state.json", root);
    txt = st_read_whole_file(path);
    if (!txt) {
        hub_log(TAG, "hub_state.json absent -> manifest defaults (E5 path)");
        return 0;
    }
    jroot = cJSON_Parse(txt);
    free(txt);
    if (!jroot) {
        hub_log(TAG, "hub_state.json CORRUPT -> rebuilt from manifest defaults (E5 pass, no crash)");
        return 0;
    }
    jplugins = cJSON_GetObjectItemCaseSensitive(jroot, "plugins");
    if (cJSON_IsObject(jplugins)) {
        for (i = 0; i < g_n_plugins; i++) {
            PluginRec* p = &g_plugins[i];
            cJSON* jp;
            cJSON* am;
            cJSON* ls;
            if (!p->id[0]) continue;
            jp = cJSON_GetObjectItemCaseSensitive(jplugins, p->id);
            if (!jp) continue;
            am = cJSON_GetObjectItemCaseSensitive(jp, "autoMount");
            if (cJSON_IsBool(am)) {
                int nv = cJSON_IsTrue(am) ? 1 : 0;
                if (nv != p->auto_mount)
                    hub_log(TAG, "'%s' autoMount override: %d -> %d (from state file)",
                            p->id, p->auto_mount, nv);
                p->auto_mount = nv;
            }
            ls = cJSON_GetObjectItemCaseSensitive(jp, "lastState");
            if (cJSON_IsString(ls))
                strncpy(p->prev_state, ls->valuestring, sizeof(p->prev_state) - 1);
        }
    }
    cJSON_Delete(jroot);
    hub_log(TAG, "hub_state.json loaded OK");
    return 1;
}

int state_save(const char* root)
{
    char path[MAX_PATH + 32];
    char tmp[MAX_PATH + 40];
    cJSON* jroot;
    cJSON* jplugs;
    SYSTEMTIME st;
    char ts[32];
    FILE* f;
    char* txt;
    int i, n = 0;

    _snprintf(path, sizeof(path), "%s\\hub_state.json", root);
    _snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    jroot = cJSON_CreateObject();
    if (!jroot) return 0;
    GetLocalTime(&st);
    _snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02d",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    cJSON_AddNumberToObject(jroot, "version", 1);
    cJSON_AddStringToObject(jroot, "updatedAt", ts);
    jplugs = cJSON_AddObjectToObject(jroot, "plugins");
    if (jplugs) {
        for (i = 0; i < g_n_plugins; i++) {
            PluginRec* p = &g_plugins[i];
            cJSON* o;
            const char* ls;
            if (!p->id[0] || p->state == PS_INVALID) continue;
            o = cJSON_AddObjectToObject(jplugs, p->id);
            if (!o) continue;
            /* S10 fix: lastState = persisted view. ACTIVE while mounted; otherwise
             * carry prev_state (explicit mount/unmount update it) so lifecycle
             * saves (init auto-mount / terminate) never downgrade the previous
             * session's ACTIVE record to REGISTERED. */
            ls = (p->state == PS_ACTIVE) ? "ACTIVE"
               : (p->prev_state[0] ? p->prev_state
                                   : registry_state_name(p->state));
            cJSON_AddBoolToObject(o, "autoMount", p->auto_mount);
            cJSON_AddStringToObject(o, "lastState", ls);
            n++;
        }
    }
    txt = cJSON_PrintUnformatted(jroot);
    cJSON_Delete(jroot);
    if (!txt) {
        hub_log(TAG, "save FAILED: cJSON print error");
        return 0;
    }
    f = fopen(tmp, "wb");
    if (!f) {
        hub_log(TAG, "save FAILED: cannot open %s", tmp);
        cJSON_free(txt);
        return 0;
    }
    fputs(txt, f);
    fclose(f);
    cJSON_free(txt);
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        hub_log(TAG, "save: MoveFileExA failed (%lu), falling back to remove+rename",
                GetLastError());
        remove(path);
        if (rename(tmp, path) != 0) {
            hub_log(TAG, "save FAILED: rename error");
            return 0;
        }
    }
    hub_log(TAG, "hub_state.json saved (%d plugin entries)", n);
    return 1;
}
