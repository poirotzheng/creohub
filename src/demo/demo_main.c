/* =====================================================================
 * creo-hub M1 演示插件 (demo.dll / demo2.dll / demo_fail.dll 同源参数化)
 * - 默认编译          : demo   (id/菜单/命令 见 demo_ids.h)
 * - /DDEMO_VARIANT_2  : demo2  (多插件并发实验 E3)
 * - /DDEMO_FAIL_VARIANT : user_initialize 必失败(返回1), 错误注入实验
 * 日志: <root>\logs\hub_m1.log  (tag = 插件id)
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

#include "demo_ids.h"

static char g_root[MAX_PATH] = "";

static const char* demo_errname(ProError e)
{
    switch (e) {
    case PRO_TK_NO_ERROR:      return "NO_ERROR";
    case PRO_TK_USER_ABORT:    return "USER_ABORT";
    case PRO_TK_BAD_CONTEXT:   return "BAD_CONTEXT";
    case PRO_TK_E_IN_USE:      return "E_IN_USE";
    case PRO_TK_E_NOT_FOUND:   return "E_NOT_FOUND";
    case PRO_TK_E_FOUND:       return "E_FOUND";
    case PRO_TK_GENERAL_ERROR: return "GENERAL_ERROR";
    case PRO_TK_MSG_NOT_FOUND: return "MSG_NOT_FOUND";
    default:                   return "OTHER";
    }
}

static void demo_detect_root(void)
{
    char modpath[MAX_PATH];
    HMODULE hm = NULL;
    char* p;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)demo_detect_root, &hm);
    GetModuleFileNameA(hm, modpath, sizeof(modpath));
    p = strstr(modpath, "\\plugins\\");
    if (p) *p = '\0';
    else GetCurrentDirectoryA(MAX_PATH, modpath);
    strcpy(g_root, modpath);
}

static void demo_log(const char* fmt, ...)
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
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, DEMO_PLUGIN_ID);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

#ifndef DEMO_FAIL_VARIANT

static int demo_hello_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    ProUIMessageButton* buttons = NULL;
    ProUIMessageButton choice = PRO_UI_MESSAGE_OK;
    (void)cmd; (void)val; (void)data;
    demo_log("hello command invoked (popup)");
    if (ProArrayAlloc(1, sizeof(ProUIMessageButton), 1, (ProArray*)&buttons) == PRO_TK_NO_ERROR) {
        buttons[0] = PRO_UI_MESSAGE_OK;
        ProUIMessageDialogDisplay(PROUIMESSAGE_INFO,
                                  (wchar_t*)L"Demo (M1)",
                                  (wchar_t*)DEMO_HELLO_MSG,
                                  buttons, PRO_UI_MESSAGE_OK, &choice);
        ProArrayFree((ProArray*)&buttons);
    }
    return 0;
}

static uiCmdAccessState demo_access(uiCmdAccessMode mode)
{
    (void)mode;
    return ACCESS_AVAILABLE;
}

#endif /* !DEMO_FAIL_VARIANT */

int user_initialize(int argc, char* argv[], char* version, char* build,
                    wchar_t errbuf[80])
{
    (void)argc; (void)argv; (void)build;
    demo_detect_root();

#ifdef DEMO_FAIL_VARIANT
    demo_log("user_initialize: simulated init failure, returning 1 (creo=%s)",
             version ? version : "?");
    wcscpy(errbuf, L"demofail: simulated init failure (M1 experiment)");
    return 1;
#else
    {
        ProFileName MsgFile;
        uiCmdCmdId hello_id = NULL;
        ProError st;
        demo_log("user_initialize start (creo=%s) root=%s",
                 version ? version : "?", g_root);
        ProUITranslationFilesEnable();
        ProStringToWstring(MsgFile, DEMO_MSG_FILE);

        st = ProMenubarMenuAdd(DEMO_MENU_NAME, DEMO_MENU_LABEL, "Help",
                               PRO_B_FALSE, MsgFile);
        demo_log("ProMenubarMenuAdd('%s') ret=%d(%s)",
                 DEMO_MENU_NAME, (int)st, demo_errname(st));

        st = ProCmdActionAdd(DEMO_CMD_NAME, (uiCmdCmdActFn)demo_hello_act,
                             uiProeImmediate, (uiCmdAccessFn)demo_access,
                             PRO_B_TRUE, PRO_B_TRUE, &hello_id);
        demo_log("ProCmdActionAdd('%s') ret=%d(%s)",
                 DEMO_CMD_NAME, (int)st, demo_errname(st));

        st = ProMenubarmenuPushbuttonAdd(DEMO_MENU_NAME, DEMO_BTN_NAME,
                                         DEMO_BTN_LABEL, DEMO_BTN_HELP,
                                         "", PRO_B_FALSE, hello_id, MsgFile);
        demo_log("ProMenubarmenuPushbuttonAdd ret=%d(%s)", (int)st, demo_errname(st));

        demo_log("user_initialize done, returning 0");
        return 0;
    }
#endif
}

void user_terminate(void)
{
#ifdef DEMO_FAIL_VARIANT
    demo_detect_root();
    demo_log("user_terminate (no-op for fail variant)");
#else
    demo_log("user_terminate: clean variant, no dialogs/threads left open");
#endif
}
