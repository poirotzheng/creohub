/* creo-hub M1: ptk-sync adapter (raw ProToolkitDllLoad/Unload calls).
 * M3b b1: + production call primitives (promoted verbatim in behavior from the
 * M3a dev_e11 experiment face; docs\23 §3.1, channel decision docs\24 §2:
 * route B = in-process GetProcAddress direct call (DEFAULT), route A =
 * ProToolkitTaskExecute (registry-declarable contrast channel). */
#ifndef ADAPTER_PTK_H
#define ADAPTER_PTK_H

#include <ProToolkit.h>
#include <ProArray.h>
#include <ProValue.h>
#include "registry.h"

/* Fills p->last_err / p->last_user_err / p->last_msg; sets p->handle on success. */
ProError adapter_ptk_mount(PluginRec* p);
/* Calls ProToolkitDllUnload(p->handle); clears handle on success. */
ProError adapter_ptk_unmount(PluginRec* p);

/* ------------------------------------------------------------ M3b call face */

/* Channel status of a call attempt (infra layer; business results live in
 * OUT_JSON - the two-layer error discipline, docs\24 §5). */
typedef enum {
    ADP_CH_OK = 0,
    ADP_CH_MODULE_NULL,      /* B: GetModuleHandleW(entry_abs) failed (not loaded?) */
    ADP_CH_PROCADDR_NULL,    /* B: export not found */
    ADP_CH_HANDLE_NULL,      /* A: p->handle NULL (not mounted) */
    ADP_CH_TASK_ERR,         /* A: ProToolkitTaskExecute transport error (fret/chbuf detail) */
    ADP_CH_ALLOC             /* input build OOM / conversion failure */
} AdapterChannelStatus;

const char* adapter_channel_name(AdapterChannelStatus s);

/* utf8<->wide helpers (caller frees with free()). NULL on failure. */
wchar_t* adapter_utf8_to_w(const char* s);
char*    adapter_w_to_utf8(const wchar_t* w);

/* IN_JSON loader (official mechanism: ProArrayAlloc + ProValuedata*Set +
 * ObjectAdd). as_wchar=0 -> PRO_VALUE_TYPE_STRING (char* UTF-8, primary form
 * per docs\24 §3); 1 -> WSTRING (backup switch). NULL on failure. */
ProArgument* adapter_build_inputs(const char* in_json, int as_wchar);

/* OUT_JSON extraction (first string value; label "OUT_JSON" wins). Returns
 * malloc'd UTF-8 text or NULL. n_out/label_seen are diagnostics.
 * NOTE: caller owns `outputs` disposal: ProArgumentProarrayFree(&outputs). */
char* adapter_extract_out_json(ProArgument* outputs, int* n_out,
                               char* label_seen, int lsz);

/* One call through route "B" (default; anything else = "A"). inputs are
 * caller-owned (free with ProArgumentProarrayFree after); *outputs filled on
 * transport success (caller frees likewise); *fret = function return (ProError
 * from the plugin entry). chbuf receives the TASK_<errname> detail for A. */
AdapterChannelStatus adapter_call(PluginRec* p, const char* route,
                                  const char* func, ProArgument* inputs,
                                  ProArgument** outputs, ProError* fret,
                                  char* chbuf, int chsz);

/* B-route export pre-validation (docs\23 §3.1): called by registry_mount right
 * after a SUCCESSFUL ProToolkitDllLoad, only for v2 packages that declare
 * commands. GetProcAddress(hubCmdExport) failure => caller marks the package
 * INVALID (never a half-dead runtime state). Returns 1 = OK; 0 with err set.
 * Lives in the adapter so offline tests can stub it (no real module here). */
int adapter_verify_export(PluginRec* p, char* err, int errsz);

#endif /* ADAPTER_PTK_H */
