/* =====================================================================
 * creo-hub M1 offline test stubs: replace hub_log.c + adapter_ptk.c so
 * registry.c / state.c can run WITHOUT Creo and WITHOUT PTK libs.
 * Stub adapter simulates M0-measured PTK behavior:
 *   - normal dll   -> NO_ERROR + fake handle
 *   - demo_fail    -> GENERAL_ERROR (M0 T6 measurement)
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <ProToolkit.h>
#include "registry.h"

char g_root[MAX_PATH] = "";
char g_dll_dir[MAX_PATH] = "";
/* M2 storm test (reconcile_test.c) sets this to suppress per-op log spam;
 * registry_test.c keeps the default 0 and is unaffected. */
int g_hub_log_quiet = 0;
/* M2 web_api test (O12): when non-NULL, hub_log also appends each line here
 * so audit-channel format can be asserted offline (production hub_log.c
 * writes logs\hub_m1.log; the file-write half is M1-verified + C rounds). */
FILE* g_hub_log_tee = NULL;
/* M3b b2 staging delays (defined here: adapter_ptk_mount below uses them) */
int g_stub_sleep_ms = 0;        /* adapter_call delay (orphan tests) */
int g_stub_mount_sleep_ms = 0;  /* adapter_ptk_mount delay (pump-busy tests) */
/* b3: successful mount order capture (depgraph_test topological assertions) */
char g_stub_mount_order[32][64];
int  g_stub_mount_n = 0;

const char* hub_errname(ProError e)
{
    switch (e) {
    case PRO_TK_NO_ERROR:      return "NO_ERROR";
    case PRO_TK_USER_ABORT:    return "USER_ABORT";
    case PRO_TK_BAD_INPUTS:    return "BAD_INPUTS";
    case PRO_TK_BAD_CONTEXT:   return "BAD_CONTEXT";
    case PRO_TK_E_IN_USE:      return "E_IN_USE";
    case PRO_TK_E_NOT_FOUND:   return "E_NOT_FOUND";
    case PRO_TK_NOT_VALID:     return "NOT_VALID";
    case PRO_TK_GENERAL_ERROR: return "GENERAL_ERROR";
    case PRO_TK_MSG_NOT_FOUND: return "MSG_NOT_FOUND";
    default:                   return "OTHER";
    }
}

void hub_detect_root(void)
{
    GetCurrentDirectoryA(MAX_PATH, g_root);
}

void hub_log(const char* tag, const char* fmt, ...)
{
    va_list ap;
    char line[2048];
    if (g_hub_log_quiet) return;
    va_start(ap, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 1] = '\0';
    printf("  [%s] %s\n", tag ? tag : "?", line);
    if (g_hub_log_tee) {
        fprintf(g_hub_log_tee, "[%s] %s\n", tag ? tag : "?", line);
        fflush(g_hub_log_tee);
    }
}

void hub_popup(int is_err, const wchar_t* title, const wchar_t* msg)
{
    printf("  [POPUP%s] %ls | %ls\n", is_err ? "-ERR" : "", title ? title : L"", msg ? msg : L"");
}

void hub_popup_result(int is_err, const char* tag, ProError ret,
                      ProError user_err, const wchar_t* user_msg)
{
    (void)user_msg;
    printf("  [POPUP-RESULT%s] %hs ret=%d(%hs) user_err=%d\n",
           is_err ? "-ERR" : "", tag ? tag : "", (int)ret, hub_errname(ret), (int)user_err);
}

/* ---------------- stub ptk-sync adapter ---------------- */

ProError adapter_ptk_mount(PluginRec* p)
{
    if (g_stub_mount_sleep_ms > 0) Sleep((DWORD)g_stub_mount_sleep_ms);
    if (strstr(p->entry_abs, "demo_fail") != NULL) {
        p->last_err = PRO_TK_GENERAL_ERROR;
        p->last_user_err = PRO_TK_GENERAL_ERROR;
        wcscpy(p->last_msg, L"simulated init failure (stub)");
        return PRO_TK_GENERAL_ERROR;
    }
    /* b3: capture successful mount order (depgraph_test topo assertions) */
    if (g_stub_mount_n < 32) {
        strncpy(g_stub_mount_order[g_stub_mount_n], p->id, 63);
        g_stub_mount_order[g_stub_mount_n][63] = '\0';
        g_stub_mount_n++;
    }
    p->handle = (ProToolkitDllHandle)(uintptr_t)0xDEAD0001;
    p->last_err = PRO_TK_NO_ERROR;
    p->last_user_err = PRO_TK_NO_ERROR;
    p->last_msg[0] = L'\0';
    return PRO_TK_NO_ERROR;
}

ProError adapter_ptk_unmount(PluginRec* p)
{
    extern int g_stub_unmount_fail;
    if (!p->handle) {
        p->last_err = PRO_TK_BAD_INPUTS;
        return PRO_TK_BAD_INPUTS;
    }
    if (g_stub_unmount_fail) {          /* E12 seam: DLL refuses to release */
        p->last_err = PRO_TK_GENERAL_ERROR;
        return PRO_TK_GENERAL_ERROR;
    }
    p->handle = NULL;
    p->last_err = PRO_TK_NO_ERROR;
    return PRO_TK_NO_ERROR;
}

int g_stub_unmount_fail = 0;

/* ---------------- M3b b1: call-face stubs (adapter call primitives +
 * the PTK model functions call_exec.c touches). Controllable via globals
 * so call_test/web_api_test can stage channel failures and OUT_JSON. */

#include "adapter_ptk.h"
#include <ProMdl.h>

AdapterChannelStatus g_stub_ch = ADP_CH_OK;      /* next adapter_call result */
char        g_stub_ch_name[64] = "OK";           /* chbuf content */
char        g_stub_out_json[8192] = "";          /* next OUT_JSON ('' = none) */
ProError    g_stub_fret = PRO_TK_NO_ERROR;       /* next function return */
wchar_t     g_stub_mdl_name[64] = L"";           /* '' = no current model */

/* ---------------- M3c c1: firmware command-behaviour dispatch ----------
 * api_serve "callfw" mode (argv[7]) turns this ON: adapter behaviour keys
 * off the command id inside the forwarded IN_JSON instead of the shared
 * staging globals, so an HTTP-driven battery (bridge_test I/J) gets four
 * behaviours without in-process staging:
 *   <pkg>.fail  -> OUT_JSON {"ok":false,...FIXTURE_FAIL}  (200 biz-fail)
 *   <pkg>.slow  -> Sleep(dispatch_slow_ms) then echo      (504/orphan w/ timeoutSec:1)
 *   default     -> OUT_JSON {"ok":true,"echo":"<IN_JSON escaped>"}
 * OFF by default: call_test / web_api_test keep the EXACT legacy semantics
 * (g_stub_out_json staging below is untouched when dispatch is off). */
int  g_stub_dispatch_on = 0;
int  g_stub_dispatch_slow_ms = 2500;
static char g_stub_in_json[8192] = "";

/* does the captured IN_JSON carry a command id ending in `suffix`? */
static int stub_cmd_suffix(const char* suffix)
{
    const char* k = strstr(g_stub_in_json, "\"command\":\"");
    const char* e;
    size_t cl, sl;
    if (!k) return 0;
    k += 11;
    e = strchr(k, '"');
    if (!e) return 0;
    cl = (size_t)(e - k);
    sl = strlen(suffix);
    return cl >= sl && memcmp(k + cl - sl, suffix, sl) == 0;
}

/* {"ok":true,"echo":"<g_stub_in_json, JSON-escaped>"} - malloc'd dup */
static char* stub_echo_json(void)
{
    size_t cap = strlen(g_stub_in_json) * 6 + 64;   /* worst: \u00xx = 6x */
    char* buf = (char*)malloc(cap);
    size_t n = 0;
    const char* s;
    if (!buf) return NULL;
    n += (size_t)_snprintf(buf + n, cap - n, "{\"ok\":true,\"echo\":\"");
    for (s = g_stub_in_json; *s && n + 8 < cap; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { buf[n++] = '\\'; buf[n++] = (char)ch; }
        else if (ch == '\n') { buf[n++] = '\\'; buf[n++] = 'n'; }
        else if (ch == '\r') { buf[n++] = '\\'; buf[n++] = 'r'; }
        else if (ch == '\t') { buf[n++] = '\\'; buf[n++] = 't'; }
        else if (ch < 0x20)  { n += (size_t)_snprintf(buf + n, cap - n, "\\u%04x", ch); }
        else buf[n++] = (char)ch;
    }
    n += (size_t)_snprintf(buf + n, cap - n, "\"}");
    buf[n] = '\0';
    return buf;
}

static char* stub_fixed_dup(const char* s)
{
    size_t len = strlen(s) + 1;
    char* dup = (char*)malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

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
    int n; wchar_t* b;
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
    int n; char* b;
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
    (void)as_wchar;
    if (g_stub_dispatch_on && in_json) {          /* M3c c1 callfw capture */
        strncpy(g_stub_in_json, in_json, sizeof(g_stub_in_json) - 1);
        g_stub_in_json[sizeof(g_stub_in_json) - 1] = '\0';
    }
    return (ProArgument*)(void*)0xB0B0;   /* fake non-NULL */
}

char* adapter_extract_out_json(ProArgument* outputs, int* n_out,
                               char* label_seen, int lsz)
{
    char* dup;
    size_t len;
    (void)outputs;
    if (g_stub_dispatch_on) {
        /* M3c c1 callfw: behaviour by command id (fail -> biz-fail OUT_JSON,
         * everything else -> echo of the forwarded IN_JSON). */
        static const char FIX_FAIL[] =
            "{\"ok\":false,\"err_code\":\"FIXTURE_FAIL\","
            "\"err_msg\":\"staged business failure (battery fixture)\","
            "\"hint\":\"p-call.fail always fails by design\"}";
        if (stub_cmd_suffix(".fail")) dup = stub_fixed_dup(FIX_FAIL);
        else                          dup = stub_echo_json();
        if (!dup) { *n_out = 0; label_seen[0] = '\0'; return NULL; }
        *n_out = 1;
        strncpy(label_seen, "OUT_JSON", lsz - 1); label_seen[lsz - 1] = '\0';
        return dup;
    }
    if (!g_stub_out_json[0]) { *n_out = 0; label_seen[0] = '\0'; return NULL; }
    *n_out = 1;
    strncpy(label_seen, "OUT_JSON", lsz - 1); label_seen[lsz - 1] = '\0';
    len = strlen(g_stub_out_json) + 1;
    dup = (char*)malloc(len);
    if (dup) memcpy(dup, g_stub_out_json, len);
    return dup;
}

AdapterChannelStatus adapter_call(PluginRec* p, const char* route,
                                  const char* func, ProArgument* inputs,
                                  ProArgument** outputs, ProError* fret,
                                  char* chbuf, int chsz)
{
    (void)p; (void)route; (void)func; (void)inputs;
    if (g_stub_sleep_ms > 0) Sleep((DWORD)g_stub_sleep_ms);   /* b2 orphan staging */
    if (g_stub_dispatch_on && stub_cmd_suffix(".slow"))        /* M3c c1 callfw */
        Sleep((DWORD)g_stub_dispatch_slow_ms);
    *outputs = (g_stub_ch == ADP_CH_OK) ? (ProArgument*)(void*)0xC0C0 : NULL;
    *fret = g_stub_fret;
    strncpy(chbuf, g_stub_ch_name, chsz - 1);
    chbuf[chsz - 1] = '\0';
    return g_stub_ch;
}

int adapter_verify_export(PluginRec* p, char* err, int errsz)
{
    (void)p;
    if (err && errsz) err[0] = '\0';
    return 1;   /* offline: export presence is call_test's staged concern */
}

/* PTK symbols call_exec.c / hub code reference (headers only offline) */
ProError ProArgumentProarrayFree(ProArgument** arr)
{
    if (arr) *arr = NULL;
    return PRO_TK_NO_ERROR;
}

ProError ProMdlCurrentGet(ProMdl* p_handle)
{
    if (!g_stub_mdl_name[0]) return PRO_TK_GENERAL_ERROR;   /* no model */
    *p_handle = (ProMdl)(void*)0xD00D;
    return PRO_TK_NO_ERROR;
}

ProError ProMdlNameGet(ProMdl handle, ProName name)
{
    (void)handle;
    wcscpy(name, g_stub_mdl_name);
    return PRO_TK_NO_ERROR;
}

ProError ProMdlTypeGet(ProMdl model, ProMdlType* p_type)
{
    (void)model;
    *p_type = PRO_PART;
    return PRO_TK_NO_ERROR;
}
