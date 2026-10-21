/* =====================================================================
 * creo-hub M1 管理器 (creo_hub.dll)
 * 架构: registry(扫描+校验+状态机) + state(hub_state.json) + adapter_ptk
 * UI:   路线A = 动态消息文件 hub_gen.txt 每插件一对 Mount/Unmount 按钮
 *       路线B = 静态按钮兜底 (Status/MountAll/UnloadAll/Save/Refresh/About)
 * 实验: E1=动态消息文件可行性  E4=user_initialize 内 autoMount
 * 日志: <root>\logs\hub_m1.log   详见 docs\06 / docs\07
 * ===================================================================== */
#include <winsock2.h>   /* MUST precede windows.h: keeps legacy winsock v1 out
                         * (httpd.h needs winsock2; windows.h alone pulls v1
                         * and every type collides) */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include <ProToolkit.h>
#include <ProArray.h>
#include <ProUICmd.h>
#include <ProMenuBar.h>
#include <ProToolkitDll.h>
#include <ProUtil.h>
#include <ProUIMessage.h>
#include <ProWchar.h>
#include <ProCore.h>
#include <ProUI.h>
#include <ProUIDialog.h>

#include "hub_log.h"
#include "registry.h"
#include "state.h"
#include "reconcile.h"
#include "httpd.h"
#include "web_api.h"
#include "hub_marsh.h"
#include "hub_token.h"

#define TAG "hub"
#define HUB_TITLE L"creo-hub M2"
#define MENU_NAME "HubMenuM1"

#define MAX_UI_PLUGINS 16   /* route A per-plugin button slots (batch ops cover more) */
static char g_ui_mount_id[MAX_UI_PLUGINS][64];    /* slot -> plugin id (resolved at click time) */
static char g_ui_unmount_id[MAX_UI_PLUGINS][64];
static int g_pending_automount = 0;   /* E4 lazy retry armed */
static int g_route_a = 0;             /* per-plugin dynamic buttons OK */

/* M2 web stack state */
static ProUITimerID g_pump_timer = NULL;
static int g_http_port = 0;

/* M2 (E6): the executor pump shared by the marshaling window and the
 * 500ms ProUITimer safety net. Both run ON THE CREO MAIN THREAD, so the
 * G1 queue never sees concurrent executors. */
static int hub_pump_once(void) { return web_api_run_pending(); }
static void hub_wakeup_post(void) { hub_marsh_post(); }

static void hub_timer_action(char* dialog, ProUITimerID tid, ProAppData appdata)
{
    (void)dialog; (void)tid; (void)appdata;
    while (hub_pump_once()) { }
    /* ProUITimer is ONE-SHOT: re-arm to keep the 500ms safety net alive
     * (X3 fallback: pumps even if a PostMessage wakeup is ever lost). */
    if (g_pump_timer)
        ProUIDialogTimerStart("hub_pump_face", g_pump_timer, 500, PRO_B_FALSE);
}

static uiCmdAccessState hub_access(uiCmdAccessMode mode)
{
    (void)mode;
    return ACCESS_AVAILABLE;
}

/* ------------------------------------------------------- lazy automount */
static void hub_lazy_automount(void)
{
    ReconcileResult rr;
    if (!g_pending_automount) return;
    g_pending_automount = 0;
    memset(&rr, 0, sizeof(rr));
    hub_log(TAG, "[E4] lazy retry triggered from action context (M2: reconcile_run)");
    reconcile_run(RECONCILE_ORDER_TOPO, 1, &rr);
    hub_log(TAG, "[E4] lazy retry done: mounted=%d failed=%d", rr.mounted, rr.failed);
    if (rr.failed > 0) {
        wchar_t b[256];
        swprintf(b, 256, L"[E4] lazy retry: %d mounted, %d failed (see log)", rr.mounted, rr.failed);
        hub_popup(1, HUB_TITLE, b);
    }
}

/* ---- route A per-plugin callbacks: slot-based, plugin id resolved AT CALL TIME.
 * Why not map by cmd-id: (1) PTK does not guarantee the callback's cmd equals the
 * registered uiCmdCmdId; (2) registry_rescan can shift array indexes, so a
 * baked-in index could hit the WRONG plugin after Refresh. Slots store the id. */
static void hub_ui_mount_by_slot(int slot)
{
    int i;
    hub_lazy_automount();
    i = registry_find_idx(g_ui_mount_id[slot]);
    if (i < 0) {
        hub_log(TAG, "mount slot %d: id '%s' no longer in registry",
                slot, g_ui_mount_id[slot]);
        hub_popup(1, HUB_TITLE,
                  L"That plugin is no longer in the registry.\nUse Refresh Registry; new packages need hub restart.");
        return;
    }
    hub_log(TAG, "action: Mount '%s' (slot %d)", g_plugins[i].id, slot);
    reconcile_set_target(i, 1);   /* G3/S10: explicit user want, survives rescans */
    registry_mount(i, 0);
}

static void hub_ui_unmount_by_slot(int slot)
{
    int i;
    hub_lazy_automount();
    i = registry_find_idx(g_ui_unmount_id[slot]);
    if (i < 0) {
        hub_log(TAG, "unmount slot %d: id '%s' no longer in registry",
                slot, g_ui_unmount_id[slot]);
        hub_popup(1, HUB_TITLE,
                  L"That plugin is no longer in the registry.\nUse Refresh Registry; new packages need hub restart.");
        return;
    }
    hub_log(TAG, "action: Unmount '%s' (slot %d)", g_plugins[i].id, slot);
    reconcile_set_target(i, 0);   /* G3/S10: explicit session unmount */
    registry_unmount(i, 0);
}

#define GEN_MOUNT_ACT(N) \
    static int hub_mount_act_##N(uiCmdCmdId c, uiCmdValue* v, void* d) \
    { (void)c; (void)v; (void)d; hub_ui_mount_by_slot(N); return 0; }
#define GEN_UNMOUNT_ACT(N) \
    static int hub_unmount_act_##N(uiCmdCmdId c, uiCmdValue* v, void* d) \
    { (void)c; (void)v; (void)d; hub_ui_unmount_by_slot(N); return 0; }

GEN_MOUNT_ACT(0) GEN_MOUNT_ACT(1) GEN_MOUNT_ACT(2) GEN_MOUNT_ACT(3)
GEN_MOUNT_ACT(4) GEN_MOUNT_ACT(5) GEN_MOUNT_ACT(6) GEN_MOUNT_ACT(7)
GEN_MOUNT_ACT(8) GEN_MOUNT_ACT(9) GEN_MOUNT_ACT(10) GEN_MOUNT_ACT(11)
GEN_MOUNT_ACT(12) GEN_MOUNT_ACT(13) GEN_MOUNT_ACT(14) GEN_MOUNT_ACT(15)
GEN_UNMOUNT_ACT(0) GEN_UNMOUNT_ACT(1) GEN_UNMOUNT_ACT(2) GEN_UNMOUNT_ACT(3)
GEN_UNMOUNT_ACT(4) GEN_UNMOUNT_ACT(5) GEN_UNMOUNT_ACT(6) GEN_UNMOUNT_ACT(7)
GEN_UNMOUNT_ACT(8) GEN_UNMOUNT_ACT(9) GEN_UNMOUNT_ACT(10) GEN_UNMOUNT_ACT(11)
GEN_UNMOUNT_ACT(12) GEN_UNMOUNT_ACT(13) GEN_UNMOUNT_ACT(14) GEN_UNMOUNT_ACT(15)

typedef int (*HubActFn)(uiCmdCmdId, uiCmdValue*, void*);
static const HubActFn g_mount_fns[MAX_UI_PLUGINS] = {
    hub_mount_act_0, hub_mount_act_1, hub_mount_act_2, hub_mount_act_3,
    hub_mount_act_4, hub_mount_act_5, hub_mount_act_6, hub_mount_act_7,
    hub_mount_act_8, hub_mount_act_9, hub_mount_act_10, hub_mount_act_11,
    hub_mount_act_12, hub_mount_act_13, hub_mount_act_14, hub_mount_act_15 };
static const HubActFn g_unmount_fns[MAX_UI_PLUGINS] = {
    hub_unmount_act_0, hub_unmount_act_1, hub_unmount_act_2, hub_unmount_act_3,
    hub_unmount_act_4, hub_unmount_act_5, hub_unmount_act_6, hub_unmount_act_7,
    hub_unmount_act_8, hub_unmount_act_9, hub_unmount_act_10, hub_unmount_act_11,
    hub_unmount_act_12, hub_unmount_act_13, hub_unmount_act_14, hub_unmount_act_15 };

/* ------------------------------------------------------ status summary */
static void hub_build_summary(wchar_t* buf, int bufsz)
{
    int i, off = 0;
    off += swprintf(buf + off, (size_t)(bufsz - off),
                    L"creo-hub M1 registry: %d package(s)\nroute A buttons: %hs\n\n",
                    g_n_plugins, g_route_a ? "YES" : "NO (route B only)");
    for (i = 0; i < g_n_plugins && i < 20; i++) {
        PluginRec* p = &g_plugins[i];
        off += swprintf(buf + off, (size_t)(bufsz - off), L"%hs | %hs %hs | %hs%hs\n",
                        p->id[0] ? p->id : "(no-id)", p->name, p->version,
                        registry_state_name(p->state),
                        p->auto_mount ? " | auto" : "");
        if ((p->state == PS_INVALID || p->state == PS_UNSUPPORTED) && p->invalid_reason[0])
            off += swprintf(buf + off, (size_t)(bufsz - off), L"    -> %hs\n", p->invalid_reason);
        else if (p->prev_state[0] && strcmp(p->prev_state, registry_state_name(p->state)) != 0)
            off += swprintf(buf + off, (size_t)(bufsz - off), L"    (prev session: %hs)\n", p->prev_state);
        if (off > bufsz - 256) break;
    }
    if (g_n_plugins > 20)
        off += swprintf(buf + off, (size_t)(bufsz - off), L"...and %d more\n", g_n_plugins - 20);
    swprintf(buf + off, (size_t)(bufsz - off), L"\nstate: %hs\\hub_state.json", g_root);
}

/* ------------------------------------------------------------- actions */
static int hub_status_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    static wchar_t buf[3072];
    (void)cmd; (void)val; (void)data;
    hub_lazy_automount();
    hub_log(TAG, "action: Status Summary");
    hub_build_summary(buf, 3072);
    hub_popup(0, HUB_TITLE, buf);
    return 0;
}

static int hub_mount_all_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    int i;
    ReconcileResult rr;
    wchar_t b[256];
    (void)cmd; (void)val; (void)data;
    hub_lazy_automount();
    hub_log(TAG, "action: Mount All (M2: targets + reconcile_run)");
    memset(&rr, 0, sizeof(rr));
    /* G3: want=1 on REGISTERED/ERROR/ACTIVE alike - an ACTIVE record with
     * want=0 (menu-mounted after panel-unmount) must NOT be surprise-unmounted. */
    for (i = 0; i < g_n_plugins; i++) {
        PluginState st = g_plugins[i].state;
        if (st == PS_REGISTERED || st == PS_ERROR || st == PS_ACTIVE)
            reconcile_set_target(i, 1);
    }
    reconcile_run(RECONCILE_ORDER_TOPO, 1, &rr);
    swprintf(b, 256, L"Mount All: %d mounted, %d failed (see log)", rr.mounted, rr.failed);
    hub_popup(rr.failed > 0, HUB_TITLE, b);
    return 0;
}

static int hub_unload_all_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    int i;
    ReconcileResult rr;
    wchar_t b[256];
    (void)cmd; (void)val; (void)data;
    hub_lazy_automount();
    hub_log(TAG, "action: Unload All (M2: targets + reconcile_run)");
    memset(&rr, 0, sizeof(rr));
    /* "nothing mounted": clear targets on every actionable record so the
     * reconcile does not retry ERROR ones (same semantics as web unload-all). */
    for (i = 0; i < g_n_plugins; i++) {
        PluginState st = g_plugins[i].state;
        if (st != PS_INVALID && st != PS_UNSUPPORTED)
            reconcile_set_target(i, 0);
    }
    reconcile_run(RECONCILE_ORDER_TOPO, 1, &rr);
    swprintf(b, 256, L"Unload All: %d unmounted, %d failed (see log)", rr.unmounted, rr.failed);
    hub_popup(rr.failed > 0, HUB_TITLE, b);
    return 0;
}

static int hub_save_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    wchar_t b[MAX_PATH + 128];
    int ok;
    (void)cmd; (void)val; (void)data;
    hub_lazy_automount();
    hub_log(TAG, "action: Save State");
    ok = state_save(g_root);
    if (ok)
        swprintf(b, MAX_PATH + 128, L"State saved:\n%hs\\hub_state.json", g_root);
    else
        swprintf(b, MAX_PATH + 128, L"State save FAILED (see log)");
    hub_popup(!ok, HUB_TITLE, b);
    return 0;
}

static int hub_refresh_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    static wchar_t buf[3072];
    (void)cmd; (void)val; (void)data;
    hub_lazy_automount();
    hub_log(TAG, "action: Refresh Registry");
    registry_rescan(g_root);
    hub_build_summary(buf, 3072);
    {
        size_t len = wcslen(buf);
        if (len < 3072 - 160)
            swprintf(buf + len, 3072 - len,
                     L"\n\nRefreshed. NOTE: per-plugin buttons of NEW packages\nappear after hub restart (Aux Apps: stop/start).");
    }
    hub_popup(0, HUB_TITLE, buf);
    return 0;
}

static int hub_about_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    wchar_t buf[768];
    (void)cmd; (void)val; (void)data;
    hub_lazy_automount();
    swprintf(buf, 768,
             L"creo-hub M2 - manifest-driven plugin manager\n\n"
             L"root: %hs\nlog:  %hs\\logs\\hub_m1.log\nstate: %hs\\hub_state.json\n\n"
             L"registry: %d package(s), route A buttons: %hs\n"
             L"web panel: %hs\n"
             L"JSON: cJSON 1.7.18 (vendored, static)",
             g_root, g_root, g_root, g_n_plugins, g_route_a ? "YES" : "NO",
             (httpd_is_running() && g_http_port > 0) ? "running (menu: Open Web Panel)"
                                                     : "NOT running (see log)");
    hub_popup(0, HUB_TITLE, buf);
    return 0;
}

/* M2: open the local web panel. Token travels via URL query ONCE (the panel
 * JS moves it to the X-Hub-Token header and scrubs the address bar); the
 * value itself is never logged. */
static int hub_webpanel_act(uiCmdCmdId cmd, uiCmdValue* val, void* data)
{
    char url[320];
    (void)cmd; (void)val; (void)data;
    hub_lazy_automount();
    if (!httpd_is_running() || !hub_token_value()[0]) {
        hub_popup(1, HUB_TITLE,
                  L"Web panel unavailable this session (httpd not running - see log).");
        return 0;
    }
    _snprintf(url, sizeof(url), "http://%s:%d/?token=%s",
              HTTPD_BIND_ADDR, g_http_port, hub_token_value());
    hub_log(TAG, "action: Open Web Panel -> http://%s:%d/?token=<redacted>",
            HTTPD_BIND_ADDR, g_http_port);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
    return 0;
}

/* --------------------------------------------- E1: dynamic msg file gen */
static void gen_write_entries(FILE* f)
{
    int i;
    /* M2 fixed entry (E1 dynamic mechanism, proven in M1): web panel button */
    fprintf(f, "BtnWebPanel\nOpen Web Panel\n#\n#\n");
    fprintf(f, "BtnWebPanelX\nOpen the creo-hub local web panel in your browser\n#\n#\n");
    for (i = 0; i < g_n_plugins; i++) {
        PluginRec* p = &g_plugins[i];
        if (!p->id[0] || p->state == PS_INVALID || p->state == PS_UNSUPPORTED) continue;
        fprintf(f, "BtnMount_%s\nMount %s\n#\n#\n", p->sid, p->id);
        fprintf(f, "BtnMountX_%s\nMount plugin %s (%s)\n#\n#\n", p->sid, p->id, p->name);
        fprintf(f, "BtnUnmount_%s\nUnmount %s\n#\n#\n", p->sid, p->id);
        fprintf(f, "BtnUnmountX_%s\nUnmount plugin %s\n#\n#\n", p->sid, p->id);
    }
}

static int hub_gen_write(void)
{
    char path[MAX_PATH + 64];
    FILE* f;
    /* primary: <text_dir>\text\usascii\hub_gen.txt (where Creo actually reads) */
    _snprintf(path, sizeof(path), "%s\\hub_text\\text\\usascii\\hub_gen.txt", g_root);
    f = fopen(path, "wb");
    if (!f) {
        hub_log(TAG, "[E1] cannot write %s (err=%lu)", path, GetLastError());
        return 0;
    }
    gen_write_entries(f);
    fclose(f);
    hub_log(TAG, "[E1] generated %s", path);
    /* mirror: text_dir root (harmless redundancy) */
    _snprintf(path, sizeof(path), "%s\\hub_text\\hub_gen.txt", g_root);
    f = fopen(path, "wb");
    if (f) {
        gen_write_entries(f);
        fclose(f);
        hub_log(TAG, "[E1] mirrored %s", path);
    }
    return 1;
}

/* ---------------------------------------------------------- entrypoints */
int user_initialize(int argc, char* argv[], char* version, char* build,
                    wchar_t errbuf[80])
{
    ProFileName MsgFile, GenFile;
    ProError st;
    uiCmdCmdId id;
    char logdir[MAX_PATH + 16];
    char aname[96], bname[96], hkey[96];
    int i, am_ok = 0, am_fail = 0, skip_dyn_btn = 0;
    (void)argc; (void)argv; (void)errbuf;

    hub_detect_root();
    _snprintf(logdir, sizeof(logdir), "%s\\logs", g_root);
    CreateDirectoryA(logdir, NULL);

    hub_log(TAG, "=== creo_hub M2 user_initialize: root=%s creo=%s build=%s ===",
            g_root, version ? version : "?", build ? build : "?");

    ProUITranslationFilesEnable();

    /* DIAG: registered exec/text paths (kept from M0, cheap + useful) */
    {
        ProPath exec_path, text_path;
        ProError e1, e2;
        exec_path[0] = L'\0';
        text_path[0] = L'\0';
        e1 = ProToolkitApplExecPathGet(exec_path);
        e2 = ProToolkitApplTextPathGet(text_path);
        hub_log(TAG, "DIAG ApplExecPathGet ret=%d(%s) path='%ls'", (int)e1, hub_errname(e1), exec_path);
        hub_log(TAG, "DIAG ApplTextPathGet ret=%d(%s) path='%ls'", (int)e2, hub_errname(e2), text_path);
    }

    /* 1) registry + persisted state */
    registry_scan(g_root);
    state_load(g_root);

    /* 2) E1: generate dynamic message file BEFORE any button registration */
    hub_gen_write();

    /* 3) menu (static, M0-proven file) */
    ProStringToWstring(MsgFile, "hub_messages.txt");
    ProStringToWstring(GenFile, "hub_gen.txt");
    st = ProMenubarMenuAdd(MENU_NAME, "HubMenuLabel", "Help", PRO_B_FALSE, MsgFile);
    hub_log(TAG, "ProMenubarMenuAdd('%s') ret=%d(%s)", MENU_NAME, (int)st, hub_errname(st));
    if (st != PRO_TK_NO_ERROR) {
        wchar_t eb[256];
        swprintf(eb, 256, L"Menu registration FAILED ret=%d (%hs) - see log DIAG lines",
                 (int)st, hub_errname(st));
        hub_popup(1, HUB_TITLE, eb);
        /* continue: actions still usable via mapkeys */
    }

    /* 4) static actions + buttons (route B baseline, always registered) */
    ProCmdActionAdd("HubStatus_Act", (uiCmdCmdActFn)hub_status_act,
                    uiProeImmediate, (uiCmdAccessFn)hub_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    ProMenubarmenuPushbuttonAdd(MENU_NAME, "HubBtnStatus", "HubBtnStatus",
                                "HubBtnStatusHelp", "", PRO_B_FALSE, id, MsgFile);

    ProCmdActionAdd("HubMountAll_Act", (uiCmdCmdActFn)hub_mount_all_act,
                    uiProeImmediate, (uiCmdAccessFn)hub_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    ProMenubarmenuPushbuttonAdd(MENU_NAME, "HubBtnMountAll", "HubBtnMountAll",
                                "HubBtnMountAllHelp", "", PRO_B_FALSE, id, MsgFile);

    ProCmdActionAdd("HubUnloadAll_Act", (uiCmdCmdActFn)hub_unload_all_act,
                    uiProeImmediate, (uiCmdAccessFn)hub_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    ProMenubarmenuPushbuttonAdd(MENU_NAME, "HubBtnUnloadAll", "HubBtnUnloadAll",
                                "HubBtnUnloadAllHelp", "", PRO_B_FALSE, id, MsgFile);

    ProCmdActionAdd("HubSave_Act", (uiCmdCmdActFn)hub_save_act,
                    uiProeImmediate, (uiCmdAccessFn)hub_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    ProMenubarmenuPushbuttonAdd(MENU_NAME, "HubBtnSave", "HubBtnSave",
                                "HubBtnSaveHelp", "", PRO_B_FALSE, id, MsgFile);

    ProCmdActionAdd("HubRefresh_Act", (uiCmdCmdActFn)hub_refresh_act,
                    uiProeImmediate, (uiCmdAccessFn)hub_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    ProMenubarmenuPushbuttonAdd(MENU_NAME, "HubBtnRefresh", "HubBtnRefresh",
                                "HubBtnRefreshHelp", "", PRO_B_FALSE, id, MsgFile);

    ProCmdActionAdd("HubAbout_Act", (uiCmdCmdActFn)hub_about_act,
                    uiProeImmediate, (uiCmdAccessFn)hub_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    ProMenubarmenuPushbuttonAdd(MENU_NAME, "HubBtnAbout", "HubBtnAbout",
                                "HubBtnAboutHelp", "", PRO_B_FALSE, id, MsgFile);

    /* M2: Open Web Panel button (label from the E1 dynamic msg file) */
    ProCmdActionAdd("HubWebPanel_Act", (uiCmdCmdActFn)hub_webpanel_act,
                    uiProeImmediate, (uiCmdAccessFn)hub_access, PRO_B_TRUE, PRO_B_TRUE, &id);
    st = ProMenubarmenuPushbuttonAdd(MENU_NAME, "BtnWebPanel", "BtnWebPanel",
                                     "BtnWebPanelX", "", PRO_B_FALSE, id, GenFile);
    hub_log(TAG, "[M2] webpanel pushbutton ret=%d(%s)", (int)st, hub_errname(st));

    /* 5) route A: per-plugin actions + dynamic-label buttons (slot-based, rescan-safe) */
    g_route_a = 1;
    {
        int slot = 0;
        for (i = 0; i < g_n_plugins; i++) {
            PluginRec* p = &g_plugins[i];
            uiCmdCmdId mid = NULL, uid = NULL;
            if (!p->id[0] || p->state == PS_INVALID || p->state == PS_UNSUPPORTED) continue;
            if (slot >= MAX_UI_PLUGINS) {
                hub_log(TAG, "[E1] UI slot cap (%d) reached: '%s' gets no per-plugin button "
                             "(route B batch ops still work)", MAX_UI_PLUGINS, p->id);
                continue;
            }
            strncpy(g_ui_mount_id[slot], p->id, sizeof(g_ui_mount_id[slot]) - 1);
            strncpy(g_ui_unmount_id[slot], p->id, sizeof(g_ui_unmount_id[slot]) - 1);

            _snprintf(aname, sizeof(aname), "HubMount_%s", p->sid);
            st = ProCmdActionAdd(aname, (uiCmdCmdActFn)g_mount_fns[slot],
                                 uiProeImmediate, (uiCmdAccessFn)hub_access,
                                 PRO_B_TRUE, PRO_B_TRUE, &mid);
            hub_log(TAG, "ProCmdActionAdd('%s') slot=%d ret=%d(%s)",
                    aname, slot, (int)st, hub_errname(st));

            _snprintf(aname, sizeof(aname), "HubUnload_%s", p->sid);
            st = ProCmdActionAdd(aname, (uiCmdCmdActFn)g_unmount_fns[slot],
                                 uiProeImmediate, (uiCmdAccessFn)hub_access,
                                 PRO_B_TRUE, PRO_B_TRUE, &uid);
            hub_log(TAG, "ProCmdActionAdd('%s') slot=%d ret=%d(%s)",
                    aname, slot, (int)st, hub_errname(st));

            if (!skip_dyn_btn) {
                _snprintf(bname, sizeof(bname), "BtnMount_%s", p->sid);
                _snprintf(hkey, sizeof(hkey), "BtnMountX_%s", p->sid);
                st = ProMenubarmenuPushbuttonAdd(MENU_NAME, bname, bname, hkey,
                                                 "", PRO_B_FALSE, mid, GenFile);
                hub_log(TAG, "[E1] pushbutton '%s' ret=%d(%s)", bname, (int)st, hub_errname(st));
                if (st != PRO_TK_NO_ERROR) {
                    g_route_a = 0;
                    skip_dyn_btn = 1;
                    hub_log(TAG, "[E1] RESULT: dynamic message-file buttons FAILED -> route B only");
                } else {
                    _snprintf(bname, sizeof(bname), "BtnUnmount_%s", p->sid);
                    _snprintf(hkey, sizeof(hkey), "BtnUnmountX_%s", p->sid);
                    st = ProMenubarmenuPushbuttonAdd(MENU_NAME, bname, bname, hkey,
                                                     "", PRO_B_FALSE, uid, GenFile);
                    hub_log(TAG, "[E1] pushbutton '%s' ret=%d(%s)", bname, (int)st, hub_errname(st));
                    if (st != PRO_TK_NO_ERROR) {
                        g_route_a = 0;
                        skip_dyn_btn = 1;
                        hub_log(TAG, "[E1] RESULT: dynamic message-file buttons FAILED -> route B only");
                    }
                }
            }
            slot++;
        }
    }

    /* 6) M2 (G3): startup convergence replaces M1 registry_automount_all.
     * reconcile_target_refresh() builds the whole target view from persisted
     * semantics (S10: autoMount + prev-session ACTIVE restore set), then
     * reconcile_run() converges. Same selection semantics as M1 E4, one code
     * path shared with the web API bulk ops. */
    reconcile_target_refresh();
    {
        ReconcileResult rr;
        int rc;
        memset(&rr, 0, sizeof(rr));
        rc = reconcile_run(RECONCILE_ORDER_TOPO, 1, &rr);
        am_ok = rr.mounted;
        am_fail = rr.failed;
        hub_log(TAG, "[M2] startup reconcile rc=%d mounted=%d unmounted=%d failed=%d skipped=%d",
                rc, rr.mounted, rr.unmounted, rr.failed, rr.skipped);
    }
    if (am_fail > 0) {
        g_pending_automount = 1;
        hub_log(TAG, "[E4] startup failures=%d -> LAZY RETRY ARMED "
                     "(will reconcile again on first hub action)", am_fail);
    } else {
        hub_log(TAG, "[E4] startup reconcile clean: mounted=%d (in-init context WORKS)", am_ok);
    }

    /* 7) M2 web stack: E6 marshaling window (main thread) -> G4 token ->
     * timer safety net -> httpd -> executor ready (LAST: the queue only
     * accepts POSTs once the whole pipeline can drain them). */
    if (!web_api_init()) {
        hub_log(TAG, "[M2] web_api_init FAILED - hub continues WITHOUT web panel");
    } else {
        hub_token_generate();
        web_api_set_token(hub_token_value());
        hub_token_write_file(g_root);
        if (!hub_marsh_init(hub_pump_once)) {
            hub_log(TAG, "[M2] E6 window FAILED - 500ms timer polling only (X3 degradation)");
        } else {
            web_api_set_wakeup(hub_wakeup_post);
        }
        {
            ProName tname;
            ProError te;
            ProStringToWstring(tname, "HubPumpTimer");
            te = ProUITimerCreate(hub_timer_action, NULL, tname, &g_pump_timer);
            hub_log(TAG, "[M2] ProUITimerCreate ret=%d(%s)", (int)te, hub_errname(te));
            if (te == PRO_TK_NO_ERROR && g_pump_timer) {
                te = ProUIDialogTimerStart("hub_pump_face", g_pump_timer, 500, PRO_B_FALSE);
                hub_log(TAG, "[M2] ProUIDialogTimerStart(500ms, one-shot re-armed) ret=%d(%s)",
                        (int)te, hub_errname(te));
                if (te != PRO_TK_NO_ERROR) {
                    ProUITimerDestroy(g_pump_timer);
                    g_pump_timer = NULL;
                }
            } else {
                g_pump_timer = NULL;
            }
        }
        /* A-5 wiring: register the API router BEFORE the listener opens.
         * (9/13 field bug: this call was missing -> httpd served static
         * panel fine but every /api/v1/* hit the httpd-level 404 fallback
         * "api not available", which is not logged - the panel silently
         * stalled at "connecting". Wiring is now asserted by
         * test\wiring_check.cmd and guarded by an httpd_start warning.) */
        httpd_set_api_handler(web_api_route);
        if (httpd_start(HTTPD_PORT_FIRST, &g_http_port)) {
            web_api_set_executor_ready(1);
            hub_log(TAG, "[M2] web stack UP: http://%s:%d (token file %s\\hub_token.txt)",
                    HTTPD_BIND_ADDR, g_http_port, g_root);
        } else {
            g_http_port = 0;
            hub_log(TAG, "[M2] httpd start FAILED (all ports busy) - hub continues "
                         "WITHOUT web panel; Creo menu management unaffected");
            hub_popup(1, HUB_TITLE,
                      L"Web panel unavailable: ports 8765-8775 all busy (see log).\n"
                      L"Plugin management via this menu still works.");
        }
    }

    hub_log(TAG, "hub user_initialize done");
    {
        wchar_t okbuf[896];
        char webline[160];
        if (httpd_is_running() && g_http_port > 0)
            _snprintf(webline, sizeof(webline), "http://%s:%d (menu: Open Web Panel)",
                      HTTPD_BIND_ADDR, g_http_port);
        else
            _snprintf(webline, sizeof(webline), "NOT running (see log)");
        swprintf(okbuf, 896,
                 L"creo-hub M2 loaded OK!\n\n"
                 L"root: %hs\nlog:  %hs\\logs\\hub_m1.log\n"
                 L"registry: %d package(s)\n"
                 L"route A per-plugin buttons: %hs\n"
                 L"web panel: %hs\n"
                 L"startup reconcile: %d mounted / %d failed%hs",
                 g_root, g_root, g_n_plugins,
                 g_route_a ? "YES" : "NO (route B)",
                 webline,
                 am_ok, am_fail,
                 g_pending_automount ? " -> lazy retry armed" : "");
        hub_popup(0, L"creo-hub M2 Ready", okbuf);
    }
    return 0;
}

void user_terminate(void)
{
    hub_log(TAG, "=== creo_hub M2 user_terminate ===");
    /* L9 lifecycle, safe order (two-phase quiesce, E9/C5 stall fix):
     * 1) stop accepting new work (executor not ready => POSTs get 503 fast)
     * 1b) wake every in-flight POST waiter NOW (503 "hub stopping") - without
     *     this the worker join at 3) would block for each waiter's full
     *     timeout (10..30s) because the pump at 2) is already gone
     * 2) stop the pump timer (no more callbacks after this)
     * 3) httpd_stop joins the worker (fast: nobody left waiting)
     * 4) only then tear down queue (closes handles), E6 window, token file */
    web_api_set_executor_ready(0);
    web_api_stop_waiters();
    if (g_pump_timer) {
        ProError te = ProUIDialogTimerStop(g_pump_timer);
        hub_log(TAG, "[M2] ProUIDialogTimerStop ret=%d(%s)", (int)te, hub_errname(te));
        te = ProUITimerDestroy(g_pump_timer);
        hub_log(TAG, "[M2] ProUITimerDestroy ret=%d(%s)", (int)te, hub_errname(te));
        g_pump_timer = NULL;
    }
    httpd_stop();
    web_api_shutdown();
    web_api_set_wakeup(NULL);
    hub_marsh_shutdown();
    /* 5) Symmetric lifecycle handoff (C5 field finding, 2026-09-13): the hub
     *    stops but Creo lives on -- unload every ACTIVE plugin so its DLL
     *    leaves the session. Without this, the next hub start re-mount hits
     *    E_IN_USE(-39) because the DLL is still resident, silently breaking
     *    autoMount-across-restart (P4). prev_state is restored to "ACTIVE"
     *    (registry_unmount clears it) so M1 restore semantics survive:
     *    manually-mounted plugins come back on the next start too.
     *    TAINTED / E_BUSY results are left as-is = honest state. */
    {
        int i;
        for (i = 0; i < g_n_plugins; i++) {
            if (g_plugins[i].state == PS_ACTIVE) {
                ProError ue = registry_unmount(i, 1);
                hub_log(TAG, "terminate: unload '%s' rc=%d(%s)", g_plugins[i].id,
                        (int)ue, hub_errname(ue));
                if (ue == PRO_TK_NO_ERROR)
                    strncpy(g_plugins[i].prev_state, "ACTIVE",
                            sizeof(g_plugins[i].prev_state) - 1);
            }
        }
    }
    hub_token_delete_file(g_root);
    state_save(g_root);
    hub_log(TAG, "creo_hub M2 terminated cleanly");
}
