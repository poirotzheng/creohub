/* =====================================================================
 * tools/plugin_tpl - M2 生命周期模块（指南 §14.0b 模块 2）
 * 四入口：user_initialize / user_terminate / user_start / user_stop。
 * 红线：init 禁模态对话框（异步版可以）；terminate 返回前注销全部
 * PTK 回调并 hub_sdk_terminate_check 自报（E12 卸载后崩溃教训）。
 * 抄改清单见同目录 README.md。
 * ===================================================================== */
#include <ProToolkit.h>
#include <ProUtil.h>
#include "hub_sdk.h"

/* 可选：插件本地诊断钩（SDK 送短 ASCII 行；不落敏感内容）。
 * 不需要就删掉 hub_sdk_set_audit 调用。 */
static void tpl_audit(const char* line)
{
    (void)line;   /* 例：转发到你自己的日志文件 */
}

int user_initialize(int argc, char* argv[], char* version, char* build,
                    wchar_t errbuf[80])
{
    (void)argc; (void)argv; (void)build; (void)errbuf;
    hub_sdk_set_audit(tpl_audit);

    /* 菜单/动作注册写在这里（需要时照 demo3_main.c 抄）：
     *   ProMenubarMenuAdd / ProCmdActionAdd / ProMenubarmenuPushbuttonAdd
     * 记住：ProUIDialogShow 禁止（冻结装载流程），用 ProUIDialogShowAsync。
     * version 指向 Creo 版本串，可用于最低版本自检。 */
    (void)version;
    return 0;   /* PRO_TK_NO_ERROR */
}

void user_terminate(void)
{
    char report[256];
    /* 1) 注销你 arm 过的全部 PTK 回调：ProUITimerRemove / 通知监听 /
     *    UI 回调 / 消息钩子——一个不留（红线②）。 */
    /* 2) 销毁全部 UI 资源（对话框/面板句柄）。 */
    /* 3) 登记簿自报：armed>0 会走审计钩落 DIRTY 行（hub 侧零内省，
     *    这是唯一自证面）。 */
    hub_sdk_terminate_check(report, sizeof(report));
}

void user_start(void) { }
void user_stop(void)  { }
