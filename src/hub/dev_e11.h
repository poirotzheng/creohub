/* =====================================================================
 * creo-hub M3a DEV-ONLY: E11 通道实验端点（docs\23 §5.5）。
 * 仅在 CREO_HUB_DEV 构建中编入（build_dev.cmd）；量产 build.cmd 不含本
 * 文件，web_api.c 的两处挂点也在 #ifdef 内——量产 DLL 二进制扫不到
 * "_e11" 字符串（DoD L17）。
 * 执行线程：G1 信封 op="_e11" → executor（Creo 主线程），与未来 /call
 * 同一条编组路——E11 测的就是这条路的通道原语。
 * ===================================================================== */
#ifndef DEV_E11_H
#define DEV_E11_H

/* 主线程执行体。payload=HTTP 请求体原文；out=响应 JSON；返回 HTTP 状态码
 * （200 常规/404 插件不存在/409 插件非 ACTIVE——E11-6 断言"状态校验先于
 * 通道"即由此体现）。idout/stateout 供审计行。 */
int dev_e11_execute(const char* payload, char* out, int outsz,
                    char* idout, char* stateout);

#endif /* DEV_E11_H */
