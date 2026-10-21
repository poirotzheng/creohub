/* creo-hub M1: shared logging / error-name / popup utilities */
#include "hub_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <ProArray.h>

char g_root[MAX_PATH] = "";
char g_dll_dir[MAX_PATH] = "";

/* b4 R1 (docs\25 b4): size-capped rotation, single generation. When the
 * primary log reaches the cap it is renamed over hub_m1.log.1 (old .1 is
 * replaced) and a fresh log is started - the only file-layer gap for Win7
 * unattended sessions. Tests may shrink the cap via this global. */
long g_hub_log_max_bytes = 4 * 1024 * 1024;

static void hub_log_rotate(const char* path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    char rot[MAX_PATH + 68];
    if (g_hub_log_max_bytes <= 0) return;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return;
    if (fad.nFileSizeHigh != 0) return;                 /* >4GB: leave alone */
    if ((long)fad.nFileSizeLow < g_hub_log_max_bytes) return;
    _snprintf(rot, sizeof(rot), "%s.1", path);
    rot[sizeof(rot) - 1] = '\0';
    /* benign race under concurrent loggers: an open handle makes the move
     * fail -> the next line retries. Never loses the current line. */
    MoveFileExA(path, rot, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
}

const char* hub_errname(ProError e)
{
    switch (e) {
    case PRO_TK_NO_ERROR:      return "NO_ERROR";
    case PRO_TK_USER_ABORT:    return "USER_ABORT";
    case PRO_TK_BAD_INPUTS:    return "BAD_INPUTS";
    case PRO_TK_BAD_CONTEXT:   return "BAD_CONTEXT";
    case PRO_TK_CANT_ACCESS:   return "CANT_ACCESS";
    case PRO_TK_INVALID_FILE:  return "INVALID_FILE";
    case PRO_TK_E_IN_USE:      return "E_IN_USE";
    case PRO_TK_E_NOT_FOUND:   return "E_NOT_FOUND";
    case PRO_TK_E_FOUND:       return "E_FOUND";
    case PRO_TK_CANT_WRITE:    return "CANT_WRITE";
    case PRO_TK_NOT_VALID:     return "NOT_VALID";
    case PRO_TK_E_BUSY:        return "E_BUSY";
    case PRO_TK_GENERAL_ERROR: return "GENERAL_ERROR";
    case PRO_TK_APP_UNLOCK:    return "APP_UNLOCK";
    case PRO_TK_MSG_NOT_FOUND: return "MSG_NOT_FOUND";
    default:                   return "OTHER";
    }
}

void hub_detect_root(void)
{
    char modpath[MAX_PATH];
    HMODULE hm = NULL;
    char* p;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)hub_detect_root, &hm);
    GetModuleFileNameA(hm, modpath, sizeof(modpath));
    strcpy(g_dll_dir, modpath);
    p = strrchr(g_dll_dir, '\\');
    if (p) *p = '\0';
    p = strstr(modpath, "\\build\\");
    if (!p) p = strstr(modpath, "\\bin\\");   /* M2 B-4 (docs\09 2.5): <root>\bin\ deploy */
    if (p) *p = '\0';
    else GetCurrentDirectoryA(MAX_PATH, modpath);
    strcpy(g_root, modpath);
}

void hub_log(const char* tag, const char* fmt, ...)
{
    char path[MAX_PATH + 64];
    va_list ap;
    FILE* f;
    SYSTEMTIME st;
    _snprintf(path, sizeof(path), "%s\\logs\\hub_m1.log", g_root);
    hub_log_rotate(path);
    f = fopen(path, "a");
    if (!f) {
        /* fallback: DLL directory, never lose logs silently */
        _snprintf(path, sizeof(path), "%s\\hub_m1_fallback.log", g_dll_dir);
        f = fopen(path, "a");
        if (!f) return;
    }
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] [%s] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, tag ? tag : "hub");
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

void hub_popup(int is_err, const wchar_t* title, const wchar_t* msg)
{
    ProUIMessageButton* buttons = NULL;
    ProUIMessageButton choice = PRO_UI_MESSAGE_OK;
    if (ProArrayAlloc(1, sizeof(ProUIMessageButton), 1, (ProArray*)&buttons) != PRO_TK_NO_ERROR)
        return;
    buttons[0] = PRO_UI_MESSAGE_OK;
    ProUIMessageDialogDisplay(is_err ? PROUIMESSAGE_ERROR : PROUIMESSAGE_INFO,
                              (wchar_t*)title, (wchar_t*)msg,
                              buttons, PRO_UI_MESSAGE_OK, &choice);
    ProArrayFree((ProArray*)&buttons);
}

void hub_popup_result(int is_err, const char* tag, ProError ret,
                      ProError user_err, const wchar_t* user_msg)
{
    wchar_t buf[512];
    swprintf(buf, 512, L"%hs\nret: %d (%hs)\nuser_error_ret: %d (%hs)\nuser_string_ret: %ls",
             tag, (int)ret, hub_errname(ret),
             (int)user_err, hub_errname(user_err),
             (user_msg && user_msg[0]) ? user_msg : L"(empty)");
    hub_popup(is_err, L"creo-hub M1", buf);
}
