/* =====================================================================
 * creo-hub M3e Q1: operator mode menu implementation (contract opmode.h).
 * Memory-only by design: hub restart returns to M0 (safe side).
 * ===================================================================== */
#include <windows.h>
#include <string.h>

#include "opmode.h"
#include "hub_log.h"

#define TAG "opmode"

static int g_mode = OPMODE_STANDARD;
static unsigned long long g_arm_until_ms = 0;   /* M2 window end (GetTickCount64) */
static CRITICAL_SECTION g_cs;
static int g_cs_inited = 0;

static void opmode_cs_init(void)
{
    if (!g_cs_inited) {
        InitializeCriticalSection(&g_cs);
        g_cs_inited = 1;
    }
}

void opmode_init(void)
{
    opmode_cs_init();
    EnterCriticalSection(&g_cs);
    g_mode = OPMODE_STANDARD;
    g_arm_until_ms = 0;
    LeaveCriticalSection(&g_cs);
}

/* caller holds g_cs */
static int effective_locked(unsigned long long* remain)
{
    unsigned long long now;
    if (g_mode != OPMODE_UNATTENDED) {
        if (remain) *remain = 0;
        return g_mode;
    }
    now = GetTickCount64();
    if (now >= g_arm_until_ms) {
        g_mode = OPMODE_STANDARD;
        g_arm_until_ms = 0;
        hub_log(TAG, "OPMODE-EXPIRE -> M0 (unattended window elapsed)");
        if (remain) *remain = 0;
        return OPMODE_STANDARD;
    }
    if (remain) *remain = g_arm_until_ms - now;
    return OPMODE_UNATTENDED;
}

int opmode_current(void)
{
    int m;
    opmode_cs_init();
    EnterCriticalSection(&g_cs);
    m = effective_locked(NULL);
    LeaveCriticalSection(&g_cs);
    return m;
}

int opmode_get(int* out_mode, unsigned long long* out_remain_ms)
{
    unsigned long long remain = 0;
    opmode_cs_init();
    EnterCriticalSection(&g_cs);
    if (out_mode) *out_mode = effective_locked(&remain);
    else effective_locked(&remain);
    LeaveCriticalSection(&g_cs);
    if (out_remain_ms) *out_remain_ms = remain;
    return 0;
}

int opmode_set(int mode, unsigned long long window_ms, int ack)
{
    opmode_cs_init();
    if (mode == OPMODE_STANDARD) {
        EnterCriticalSection(&g_cs);
        g_mode = OPMODE_STANDARD;
        g_arm_until_ms = 0;
        LeaveCriticalSection(&g_cs);
        hub_log(TAG, "OPMODE-ARM mode=M0 (standard; human approval for confirm+destructive)");
        return 0;
    }
    if (mode == OPMODE_TRUST) {
        EnterCriticalSection(&g_cs);
        g_mode = OPMODE_TRUST;
        g_arm_until_ms = 0;
        LeaveCriticalSection(&g_cs);
        hub_log(TAG, "OPMODE-ARM mode=M1 (session trust: confirm auto-pass, destructive panel)");
        return 0;
    }
    if (mode == OPMODE_UNATTENDED) {
        if (window_ms == 0 || !ack) return -1;   /* explicit ack mandatory */
        EnterCriticalSection(&g_cs);
        g_mode = OPMODE_UNATTENDED;
        g_arm_until_ms = GetTickCount64() + window_ms;
        LeaveCriticalSection(&g_cs);
        hub_log(TAG, "OPMODE-ARM mode=M2 unattended window_ms=%llu ack=1 (confirm+destructive auto-pass until expiry)",
                window_ms);
        return 0;
    }
    return -1;
}

int opmode_allows(const char* risk)
{
    int m;
    if (!risk) return 0;
    opmode_cs_init();
    EnterCriticalSection(&g_cs);
    m = effective_locked(NULL);
    LeaveCriticalSection(&g_cs);
    if (m == OPMODE_TRUST)
        return strcmp(risk, "confirm") == 0;
    if (m == OPMODE_UNATTENDED)
        return strcmp(risk, "confirm") == 0 || strcmp(risk, "destructive") == 0;
    return 0;
}
