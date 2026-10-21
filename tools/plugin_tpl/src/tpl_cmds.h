#ifndef TPL_CMDS_H
#define TPL_CMDS_H
/* M3 命令实现模块的对外原型（供 M4 分发表引用）。 */
#include "cJSON.h"
#include "hub_sdk.h"

cJSON* tpl_cmd_status(const cJSON* args, HubCmdCtx* ctx, char* hint);

#endif
