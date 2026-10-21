/* =====================================================================
 * creo-hub M3a 实验插件 demoslot（E12 载体，docs\23 §5.4）
 * 目的：脏卸载缺口实测——init 里 arm 一个 1s 周期 ProUITimer（回调写日志
 * 自证活着），terminate **故意不撤**（不 Stop 不 Destroy）。卸载后定时器
 * 回调仍在已 DllUnload 的代码地址上触发 → 记录结局三选一：
 *   ① BAD_CONTEXT 类日志/Creo 拦截   ② 崩溃（预案见操作单 §9.1 S8）
 *   ③ 静默（定时器随卸载失效）
 * 注意：定时器名会话内唯一——同会话重挂会得 PRO_TK_E_IN_USE（预期内，
 * 日志记录即可；E12 本来就安排在会话尾）。
 * 日志: <root>\logs\hub_m1.log  (tag = demoslot)
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <ProToolkit.h>
#include <ProUIDialog.h>
#include <ProUtil.h>
#include <ProWchar.h>

#define DEMOSLOT_ID "demoslot"

static char g_root[MAX_PATH] = "";
static ProUITimerID g_timer = NULL;
static long g_ticks = 0;

static void slot_detect_root(void)
{
    char modpath[MAX_PATH];
    HMODULE hm = NULL;
    char* p;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)slot_detect_root, &hm);
    GetModuleFileNameA(hm, modpath, sizeof(modpath));
    p = strstr(modpath, "\\plugins\\");
    if (p) *p = '\0';
    else GetCurrentDirectoryA(MAX_PATH, modpath);
    strcpy(g_root, modpath);
}

static void slot_log(const char* fmt, ...)
{
    char path[MAX_PATH + 32];
    va_list ap;
    FILE* f;
    SYSTEMTIME st;
    _snprintf(path, sizeof(path), "%s\\logs\\hub_m1.log", g_root);
    f = fopen(path, "a");
    if (!f) return;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] [%s] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, DEMOSLOT_ID);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static void slot_timer_cb(char* dialog, ProUITimerID tid, ProAppData appdata)
{
    (void)dialog; (void)tid; (void)appdata;
    g_ticks++;
    slot_log("timer tick #%ld (this callback lives INSIDE demoslot.dll - E12)", g_ticks);
    /* 一次性定时器：回调内重新 arm（hub_main 同款姿势） */
    if (g_timer)
        ProUIDialogTimerStart("demoslot_face", g_timer, 1000, PRO_B_FALSE);
}

int user_initialize(int argc, char* argv[], char* version, char* build,
                    wchar_t errbuf[80])
{
    ProName tname;
    ProError st;
    (void)argc; (void)argv; (void)build; (void)errbuf;
    slot_detect_root();
    slot_log("user_initialize start (creo=%s) root=%s", version ? version : "?", g_root);

    ProStringToWstring(tname, "DemoSlotTick");
    st = ProUITimerCreate(slot_timer_cb, NULL, tname, &g_timer);
    slot_log("ProUITimerCreate('DemoSlotTick') ret=%d", (int)st);
    if (st == PRO_TK_NO_ERROR && g_timer) {
        st = ProUIDialogTimerStart("demoslot_face", g_timer, 1000, PRO_B_FALSE);
        slot_log("ProUIDialogTimerStart(1000ms) ret=%d", (int)st);
        if (st != PRO_TK_NO_ERROR) {
            ProUITimerDestroy(g_timer);
            g_timer = NULL;
        }
    } else {
        g_timer = NULL;   /* E_IN_USE（同会话重挂）等：记录后继续，插件仍 ACTIVE */
    }
    slot_log("user_initialize done, returning 0");
    return 0;
}

void user_terminate(void)
{
    /* E12 实验本体：故意不清理——不 ProUIDialogTimerStop、不 ProUITimerDestroy。
     * 卸载后 tick 日志若继续出现/崩溃/静默，都是要记录的实测结局。 */
    slot_log("user_terminate: INTENTIONALLY leaving timer armed (ticks so far=%ld) "
             "- E12 dirty-unload experiment, docs\\23 5.4", g_ticks);
}
