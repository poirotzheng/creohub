/* =====================================================================
 * creo-hub M2: hub_marsh.c - E6 marshaling window (see hub_marsh.h).
 * WM_APP+1 = pump request (coalescing-safe: the WndProc drains until dry,
 * so N posts collapsing into fewer messages never lose work).
 * WM_APP+2 = destroy request (lets hub_marsh_shutdown work from any
 * thread: SendMessage executes DestroyWindow ON the owner thread).
 * ===================================================================== */
#include "hub_marsh.h"
#include "hub_log.h"

#define TAG "MARSH"
#define WM_HUB_PUMP    (WM_APP + 1)
#define WM_HUB_DESTROY (WM_APP + 2)
#define MARSH_CLASS    "CreoHubMarshWndM2"

static HWND               g_hwnd = NULL;
static DWORD              g_tid  = 0;
static HINSTANCE          g_hi   = NULL;
static hub_marsh_pump_fn  g_fn   = NULL;

static LRESULT CALLBACK marsh_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_HUB_PUMP) {
        if (g_fn) {
            while (g_fn()) { /* drain */ }
        }
        return 0;
    }
    if (m == WM_HUB_DESTROY) {
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int hub_marsh_init(hub_marsh_pump_fn fn)
{
    WNDCLASSA wc;
    if (g_hwnd) {
        hub_log(TAG, "init ignored: already running on thread %lu", g_tid);
        return 1;
    }
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)marsh_wndproc, &g_hi)) {
        hub_log(TAG, "GetModuleHandleEx failed (%lu)", GetLastError());
        return 0;
    }
    g_fn = fn;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = marsh_wndproc;
    wc.hInstance     = g_hi;
    wc.lpszClassName = MARSH_CLASS;
    if (!RegisterClassA(&wc)) {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            hub_log(TAG, "RegisterClass failed (%lu)", e);
            g_fn = NULL;
            return 0;
        }
    }
    g_hwnd = CreateWindowExA(0, MARSH_CLASS, "creo-hub marsh", 0,
                             0, 0, 0, 0, HWND_MESSAGE, NULL, g_hi, NULL);
    if (!g_hwnd) {
        hub_log(TAG, "CreateWindowEx(HWND_MESSAGE) failed (%lu)", GetLastError());
        UnregisterClassA(MARSH_CLASS, g_hi);
        g_fn = NULL;
        return 0;
    }
    g_tid = GetCurrentThreadId();
    hub_log(TAG, "E6 marshaling window created on thread %lu (pump=WM_APP+1)", g_tid);
    return 1;
}

int hub_marsh_post(void)
{
    if (!g_hwnd) return 0;
    if (!PostMessageA(g_hwnd, WM_HUB_PUMP, 0, 0)) {
        hub_log(TAG, "PostMessage failed (%lu) - 500ms fallback timer will pump",
                GetLastError());
        return 0;
    }
    return 1;
}

void hub_marsh_shutdown(void)
{
    if (!g_hwnd) return;
    if (GetCurrentThreadId() == g_tid) {
        DestroyWindow(g_hwnd);
    } else {
        /* execute DestroyWindow on the owner thread (blocking) */
        SendMessageA(g_hwnd, WM_HUB_DESTROY, 0, 0);
    }
    g_hwnd = NULL;
    g_fn = NULL;
    UnregisterClassA(MARSH_CLASS, g_hi);
    hub_log(TAG, "marshaling window destroyed, class unregistered");
}

int hub_marsh_is_main_thread(void)
{
    return g_hwnd != NULL && GetCurrentThreadId() == g_tid;
}
