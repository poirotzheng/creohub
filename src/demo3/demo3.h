/* creo-hub M3a: demo3 实验插件共享定义（docs\23 §5.3） */
#ifndef DEMO3_H
#define DEMO3_H

#define DEMO3_ID        "demo3"
#define DEMO3_MENU_NAME "Demo3MenuM3A"
#define DEMO3_MSG_FILE  "demo3_messages.txt"

/* 写 <root>\logs\hub_m1.log，tag=demo3（与 demo 同沟道，probe/人工都可 tail） */
void demo3_log(const char* fmt, ...);

#endif /* DEMO3_H */
