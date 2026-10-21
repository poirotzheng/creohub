/* creo-hub M1: ptk-sync adapter - raw ProToolkitDllLoad/Unload.
 * M0-proven calling pattern: app_name wide, exec/text narrow char*, user_msg ProPath.
 * M3b b1: production call primitives (promoted from M3a dev_e11; behavior
 * identical - E11 measured routes reused verbatim, docs\24 §2). */
#include "adapter_ptk.h"
#include "hub_log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ProWchar.h>
#include <ProUtil.h>   /* ProStringToWstring */

#define TAG "ADAPTER"

ProError adapter_ptk_mount(PluginRec* p)
{
    ProError err, user_err = PRO_TK_NO_ERROR;
    ProPath user_msg;
    wchar_t wid[64];
    ProToolkitDllHandle h = NULL;

    user_msg[0] = L'\0';
    ProStringToWstring(wid, p->id);

    err = ProToolkitDllLoad(wid, p->entry_abs, p->text_abs,
                            p->user_display ? PRO_B_TRUE : PRO_B_FALSE,
                            &h, &user_err, user_msg);
    hub_log(TAG, "ProToolkitDllLoad('%s', entry='%s', text='%s', display=%d) "
                 "ret=%d(%s) user_err=%d(%s) msg='%ls' handle=%p",
            p->id, p->entry_abs, p->text_abs, p->user_display,
            (int)err, hub_errname(err),
            (int)user_err, hub_errname(user_err), user_msg, (void*)h);

    p->last_err = err;
    p->last_user_err = user_err;
    wcscpy(p->last_msg, user_msg);
    /* only trust handle on success (M0: fail case returned FFFFFFFFFFFFFFFF) */
    if (err == PRO_TK_NO_ERROR && h != NULL)
        p->handle = h;
    return err;
}

ProError adapter_ptk_unmount(PluginRec* p)
{
    ProError err;
    if (!p->handle) {
        hub_log(TAG, "ProToolkitDllUnload('%s') skipped: null handle", p->id);
        p->last_err = PRO_TK_BAD_INPUTS;
        return PRO_TK_BAD_INPUTS;
    }
    err = ProToolkitDllUnload(p->handle);
    hub_log(TAG, "ProToolkitDllUnload('%s', handle=%p) ret=%d(%s)",
            p->id, (void*)p->handle, (int)err, hub_errname(err));
    p->last_err = err;
    if (err == PRO_TK_NO_ERROR)
        p->handle = NULL;
    return err;
}

/* ------------------------------------------------------------ M3b call face */

const char* adapter_channel_name(AdapterChannelStatus s)
{
    switch (s) {
    case ADP_CH_OK:           return "OK";
    case ADP_CH_MODULE_NULL:  return "MODULE_NULL";
    case ADP_CH_PROCADDR_NULL:return "PROCADDR_NULL";
    case ADP_CH_HANDLE_NULL:  return "HANDLE_NULL";
    case ADP_CH_TASK_ERR:     return "TASK_ERR";
    case ADP_CH_ALLOC:        return "ALLOC";
    default:                  return "UNKNOWN";
    }
}

wchar_t* adapter_utf8_to_w(const char* s)
{
    int n;
    wchar_t* b;
    if (!s) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    b = (wchar_t*)malloc((size_t)n * sizeof(wchar_t));
    if (!b) return NULL;
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, b, n) <= 0) { free(b); return NULL; }
    return b;
}

char* adapter_w_to_utf8(const wchar_t* w)
{
    int n;
    char* b;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    b = (char*)malloc((size_t)n);
    if (!b) return NULL;
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, b, n, NULL, NULL) <= 0) { free(b); return NULL; }
    return b;
}

ProArgument* adapter_build_inputs(const char* in_json, int as_wchar)
{
    ProArgument* in = NULL;
    ProArgument arg;
    ProError st;
    st = ProArrayAlloc(0, sizeof(ProArgument), 1, (ProArray*)&in);
    if (st != PRO_TK_NO_ERROR) return NULL;
    memset(&arg, 0, sizeof(arg));
    wcscpy(arg.label, L"IN_JSON");
    if (as_wchar) {
        wchar_t* w = adapter_utf8_to_w(in_json ? in_json : "");
        if (!w) { ProArgumentProarrayFree(&in); return NULL; }
        st = ProValuedataWstringSet(&arg.value, w);
        free(w);
    } else {
        st = ProValuedataStringSet(&arg.value, (char*)(in_json ? in_json : ""));
    }
    if (st != PRO_TK_NO_ERROR) { ProArgumentProarrayFree(&in); return NULL; }
    st = ProArrayObjectAdd((ProArray*)&in, -1, 1, &arg);
    if (st != PRO_TK_NO_ERROR) { ProArgumentProarrayFree(&in); return NULL; }
    return in;
}

static char* adp_dup8(const char* s)
{
    size_t n; char* b;
    if (!s) return NULL;
    n = strlen(s) + 1;
    b = (char*)malloc(n);
    if (b) memcpy(b, s, n);
    return b;
}

char* adapter_extract_out_json(ProArgument* outputs, int* n_out,
                               char* label_seen, int lsz)
{
    int n = 0, i;
    char* labeled = NULL;
    char* any = NULL;
    label_seen[0] = '\0';
    if (!outputs) { *n_out = 0; return NULL; }
    if (ProArraySizeGet((ProArray)outputs, &n) != PRO_TK_NO_ERROR) { *n_out = 0; return NULL; }
    *n_out = n;
    for (i = 0; i < n; i++) {
        ProValueData* v = &outputs[i].value;
        char* txt = NULL;
        if (i == 0) {
            char* lb = adapter_w_to_utf8(outputs[i].label);
            if (lb) { strncpy(label_seen, lb, lsz - 1); label_seen[lsz - 1] = '\0'; free(lb); }
        }
        if (v->type == PRO_VALUE_TYPE_STRING && v->v.s) txt = adp_dup8(v->v.s);
        else if (v->type == PRO_VALUE_TYPE_WSTRING && v->v.w) txt = adapter_w_to_utf8(v->v.w);
        if (!txt) continue;
        if (wcscmp(outputs[i].label, L"OUT_JSON") == 0) { free(any); free(labeled); labeled = txt; break; }
        if (!any) any = txt; else free(txt);
    }
    if (labeled) { free(any); return labeled; }
    return any;
}

AdapterChannelStatus adapter_call(PluginRec* p, const char* route,
                                  const char* func, ProArgument* inputs,
                                  ProArgument** outputs, ProError* fret,
                                  char* chbuf, int chsz)
{
    *outputs = NULL;
    *fret = PRO_TK_NO_ERROR;
    chbuf[0] = '\0';

    if (route && strcmp(route, "A") == 0) {
        ProError terr;
        if (!p->handle) {
            _snprintf(chbuf, chsz, "HANDLE_NULL");
            return ADP_CH_HANDLE_NULL;
        }
        terr = ProToolkitTaskExecute(p->handle, (char*)func, inputs, outputs, fret);
        if (terr != PRO_TK_NO_ERROR) {
            _snprintf(chbuf, chsz, "TASK_%s", hub_errname(terr));
            return ADP_CH_TASK_ERR;
        }
        _snprintf(chbuf, chsz, "OK");
        return ADP_CH_OK;
    }

    /* route B (default): in-process GetProcAddress direct call */
    {
        HMODULE hm;
        FARPROC fp;
        ProTkdllFunction fn;
        wchar_t* wpath = adapter_utf8_to_w(p->entry_abs);
        hm = wpath ? GetModuleHandleW(wpath) : NULL;
        free(wpath);
        if (!hm) {
            _snprintf(chbuf, chsz, "MODULE_NULL");
            return ADP_CH_MODULE_NULL;
        }
        fp = GetProcAddress(hm, func);
        if (!fp) {
            _snprintf(chbuf, chsz, "PROCADDR_NULL");
            return ADP_CH_PROCADDR_NULL;
        }
        fn = (ProTkdllFunction)fp;
        *fret = fn(inputs, outputs);
        _snprintf(chbuf, chsz, "OK");
        return ADP_CH_OK;
    }
}

int adapter_verify_export(PluginRec* p, char* err, int errsz)
{
    HMODULE hm;
    FARPROC fp;
    wchar_t* w;
    const char* func;

    func = p->hub_cmd_export[0] ? p->hub_cmd_export : "hub_cmd";
    w = adapter_utf8_to_w(p->entry_abs);
    hm = w ? GetModuleHandleW(w) : NULL;
    free(w);
    if (!hm) {
        _snprintf(err, errsz, "module handle unavailable right after mount (entry='%s')",
                  p->entry_abs);
        err[errsz - 1] = '\0';
        return 0;
    }
    fp = GetProcAddress(hm, func);
    if (!fp) {
        _snprintf(err, errsz, "hubCmdExport '%s' not found in %s "
                              "(v2 package with commands must export it)",
                  func, p->entry_abs);
        err[errsz - 1] = '\0';
        return 0;
    }
    return 1;
}
