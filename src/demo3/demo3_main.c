/* =====================================================================
 * creo-hub M3a 实验插件 demo3（E11 载体，docs\23 §5.3）
 * - 管理面：DEMO3 菜单 + 状态按钮（E13 快照 diff 用：卸载后须消失）
 * - 命令面：hub_cmd 导出 = hub_sdk v0 一行转发（SDK 首个用户=dogfood）
 *   命令实现在 demo3_adapter.c（demo3.echo/slow/fail/boom）
 * - 清洁装卸契约：无定时器/无对话框/无监听器遗留
 * 日志: <root>\logs\hub_m1.log  (tag = demo3)
 * ===================================================================== */
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include <ProToolkit.h>
#include <ProArray.h>
#include <ProUICmd.h>
#include <ProMenuBar.h>
#include <ProUtil.h>
#include <ProUIMessage.h>
#include <ProWchar.h>
#include <ProUI.h>

#include "demo3.h"
#include "hub_sdk.h"   /* b2 R2: hub_sdk_terminate_check in user_terminate */

static char g_root[MAX_PATH] = "";

static const char* demo3_errname(ProError e)
{
    switch (e) {
    case PRO_TK_NO_ERROR:      return "NO_ERROR";
    case PRO_TK_BAD_INPUTS:    return "BAD_INPUTS";
    case PRO_TK_BAD_CONTEXT:   return "BAD_CONTEXT";
    case PRO_TK_E_IN_USE:      return "E_IN_USE";
    case PRO_TK_E_NOT_FOUND:   return "E_NOT_FOUND";
    case PRO_TK_GENERAL_ERROR: return "GENERAL_ERROR";
    case PRO_TK_MSG_NOT_FOUND: return "MSG_NOT_FOUND";
    default:                   return "OTHER";
    }
}

static void demo3_detect_root(void)
{
    char modpath[MAX_PATH];
    HMODULE hm = NULL;
    char* p;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)demo3_detect_root, &hm);
    GetModuleFileNameA(hm, modpath, sizeof(modpath));
    p = strstr(modpath, "\\plugins\\");
    if (p) *p = '\0';
    else GetCurrentDirectoryA(MAX_PATH, modpath);
    strcpy(g_root, modpath);
}

void demo3_log(const char* fmt, ...)
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
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, DEMO3_ID);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

static int demo3_status_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    ProUIMessageButton* buttons = NULL;
    ProUIMessageButton choice = PRO_UI_MESSAGE_OK;
    (void)cmd; (void)val; (void)data;
    demo3_log("status command invoked (popup)");
    if (ProArrayAlloc(1, sizeof(ProUIMessageButton), 1, (ProArray*)&buttons) == PRO_TK_NO_ERROR) {
        buttons[0] = PRO_UI_MESSAGE_OK;
        ProUIMessageDialogDisplay(PROUIMESSAGE_INFO,
                                  (wchar_t*)L"demo3 (M3a)",
                                  (wchar_t*)L"demo3 experiment plugin ACTIVE.\n"
                                  L"Commands run through hub call channel (E11).\n"
                                  L"Log: logs\\hub_m1.log tag=demo3",
                                  buttons, PRO_UI_MESSAGE_OK, &choice);
        ProArrayFree((ProArray*)&buttons);
    }
    return 0;
}

static uiCmdAccessState demo3_access(uiCmdAccessMode mode)
{
    (void)mode;
    return ACCESS_AVAILABLE;
}

/* b2: SDK audit hook -> demo3 log (dispatch HIT/MISS, R2 terminate lines) */
static void demo3_sdk_audit(const char* line)
{
    demo3_log("sdk-audit: %s", line ? line : "?");
}

int user_initialize(int argc, char* argv[], char* version, char* build,
                    wchar_t errbuf[80])
{
    ProFileName MsgFile;
    uiCmdCmdId status_id = NULL;
    ProError st;
    (void)argc; (void)argv; (void)build; (void)errbuf;
    demo3_detect_root();
    demo3_log("user_initialize start (creo=%s) root=%s", version ? version : "?", g_root);
    hub_sdk_set_audit(demo3_sdk_audit);

    ProUITranslationFilesEnable();
    ProStringToWstring(MsgFile, DEMO3_MSG_FILE);

    st = ProMenubarMenuAdd(DEMO3_MENU_NAME, "Demo3MenuLabel", "Help",
                           PRO_B_FALSE, MsgFile);
    demo3_log("ProMenubarMenuAdd('%s') ret=%d(%s)",
              DEMO3_MENU_NAME, (int)st, demo3_errname(st));

    st = ProCmdActionAdd("demo3.status", (uiCmdCmdActFn)demo3_status_act,
                         uiProeImmediate, (uiCmdAccessFn)demo3_access,
                         PRO_B_TRUE, PRO_B_TRUE, &status_id);
    demo3_log("ProCmdActionAdd('demo3.status') ret=%d(%s)", (int)st, demo3_errname(st));

    st = ProMenubarmenuPushbuttonAdd(DEMO3_MENU_NAME, "Demo3Btn",
                                     "Demo3BtnLabel", "Demo3BtnHelp",
                                     "", PRO_B_FALSE, status_id, MsgFile);
    demo3_log("ProMenubarmenuPushbuttonAdd ret=%d(%s)", (int)st, demo3_errname(st));

    demo3_log("user_initialize done, returning 0 (hub_cmd export armed)");
    return 0;
}

void user_terminate(void)
{
    /* 清洁装卸契约：demo3 不 arm 任何 PTK 回调面之外的东西；菜单/命令注册
     * 由 Creo 随 DllUnload 回收（E13 验证此假设）。
     * M3b R2：SDK 回调登记簿自报——terminate 时若仍持有 armed 回调，落日志
     * 供指南 v3 清理清单核对（demo3 预期恒为 0）。 */
    char report[256];
    int armed = hub_sdk_terminate_check(report, sizeof(report));
    if (armed > 0)
        demo3_log("user_terminate: WARNING %d callback(s) still armed: %s", armed, report);
    else
        demo3_log("user_terminate: clean, nothing self-armed to release");
}
