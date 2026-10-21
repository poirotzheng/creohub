/* =====================================================================
 * creo-hub job progress slot - SHARED CONTRACT (SDK <-> hub)
 *
 * M4b 设计（docs\39 §3.3）：async 长任务的进度通道=命名共享内存。
 * 原因（docs\39 C2）：_ctx 是 JSON 文本，无法携带内存句柄；hub DLL 只导出
 * user_initialize/user_terminate，插件侧无法 GetProcAddress 取 hub 函数。
 *
 * 所有权：
 *   - 创建者/命名者 = hub（每 job 一个 4KB 映射，名字经 _ctx.progress_map
 *     传给插件；例：Local\creo_hub_job_<pid>_<jobid>）。
 *   - 写者 = 插件侧 hub_sdk_progress()（唯一写者，主线程）。
 *   - 读者 = hub httpd worker（GET /api/v1/jobs，不经主线程）。
 *
 * seq 协议（单写者多读者，防撕裂读）：
 *   writer: seq++ (odd) -> 写 state/pct/tick/msg -> seq++ (even)
 *   reader: 读 seq (even) -> 读字段 -> 再读 seq，两次相同才采信，否则重读（最多 3 次）
 *   MSVC x86/x64 的 volatile 具备 acquire/release 语义，无需额外 barrier。
 *
 * 兼容性：magic 不匹配=旧/异版本槽 -> 双方一律按"无进度"处理（no-op，
 * 绝不影响业务结果）。state 终态只由 hub 写（插件只写 WORKING）。
 * ===================================================================== */
#ifndef HUB_JOBSLOT_H
#define HUB_JOBSLOT_H

#define HUB_JOBSLOT_MAGIC  0x484A4231u   /* 'HJB1' */
#define HUB_JOBSLOT_BYTES  256           /* 只读头 + msg（与 4KB 映射页对齐留白） */

/* state 枚举（插件只写 WORKING；终态由 hub 写） */
#define HUB_JOBSLOT_ST_NONE     0
#define HUB_JOBSLOT_ST_WORKING  1
#define HUB_JOBSLOT_ST_DONE     2
#define HUB_JOBSLOT_ST_FAILED   3
#define HUB_JOBSLOT_ST_CANCELLED 4

#define HUB_JOBSLOT_PCT_UNKNOWN (-1)

typedef struct HubJobSlot {
    unsigned long          magic;    /* = HUB_JOBSLOT_MAGIC */
    volatile unsigned long seq;      /* 奇=写入中，偶=稳定 */
    volatile int           state;    /* HUB_JOBSLOT_ST_* */
    volatile int           pct;      /* -1 或 0..100 */
    volatile unsigned long tick_ms;  /* GetTickCount() 低 32 位（仅诊断） */
    char                   msg[192]; /* UTF-8，截断；空串=无消息 */
} HubJobSlot;

#endif /* HUB_JOBSLOT_H */
