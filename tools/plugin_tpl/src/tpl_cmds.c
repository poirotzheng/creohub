/* =====================================================================
 * tools/plugin_tpl - M3 命令实现模块（指南 §14.0b 模块 3）
 * 每个命令一个业务函数：
 *   成功 = 返回 result JSON（cJSON 对象，SDK 负责序列化/释放）；
 *   失败 = 填 hint（写给模型自愈，质量=自愈率）+ 可改 ctx->err_code，返回 NULL。
 * 通道层错误码由 SDK 内建，这里永远不碰 ProArgument（双层错误纪律）。
 * ===================================================================== */
#include <ProToolkit.h>
#include "cJSON.h"
#include "hub_sdk.h"
#include "tpl_cmds.h"

/* 铁律 2 的最小只读诊断命令：抄改时保留一条同型只读命令。 */
cJSON* tpl_cmd_status(const cJSON* args, HubCmdCtx* ctx, char* hint)
{
    cJSON* r;
    (void)args;   /* 无参命令；有参用 hub_sdk_str/num/bool 取值 */
    (void)hint;
    r = cJSON_CreateObject();
    if (!r) return NULL;
    cJSON_AddStringToObject(r, "package", "tpl");
    cJSON_AddNumberToObject(r, "sdk_version", HUB_SDK_VERSION);
    cJSON_AddNumberToObject(r, "budget_ms", ctx->timeout_budget_ms);
    /* 长命令协作让路范式（L2）：
     *   while (...) { if (hub_sdk_time_left_ms(ctx) == 0) { 收尾/诚实失败 } }
     * 摸模型的命令用 hub_sdk_active_part(&err, hint) 取零件上下文。 */
    return r;
}
