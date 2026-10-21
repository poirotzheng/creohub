/* =====================================================================
 * tools/plugin_tpl - M4 分发与导出模块（指南 §14.0b 模块 4，铁律 3）
 * 分发表末项必须 { NULL, NULL } 收口；hub_cmd 一行转发 SDK 主入口。
 * 命令 id 必须带命名空间前缀 "<plugin.json 的 id>."。
 * ===================================================================== */
#include <ProToolkit.h>
#include "hub_sdk.h"
#include "tpl_cmds.h"

static const HubCmdEntry G_CMDS[] = {
    { "tpl.status", tpl_cmd_status },
    { NULL, NULL }
};

PRO_TK_DLL_EXPORT ProError hub_cmd(ProArgument* in, ProArgument** out)
{
    return hub_sdk_entry(in, out, G_CMDS);
}
