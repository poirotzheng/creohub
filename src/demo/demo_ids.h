/* creo-hub M1: demo plugin identity variants (demo / demo2).
 * demo_fail variant is controlled separately by DEMO_FAIL_VARIANT. */
#ifndef DEMO_IDS_H
#define DEMO_IDS_H

#ifdef DEMO_VARIANT_2
#define DEMO_PLUGIN_ID  "demo2"
#define DEMO_MENU_NAME  "Demo2MenuM1"
#define DEMO_MENU_LABEL "Demo2MenuLabel"
#define DEMO_CMD_NAME   "demo2.hello"
#define DEMO_BTN_NAME   "Demo2HelloBtn"
#define DEMO_BTN_LABEL  "Demo2BtnLabel"
#define DEMO_BTN_HELP   "Demo2BtnHelp"
#define DEMO_MSG_FILE   "demo2_messages.txt"
#define DEMO_HELLO_MSG  L"Hello from demo2! Multi-plugin concurrency works (E3)."
#else
#define DEMO_PLUGIN_ID  "demo"
#define DEMO_MENU_NAME  "DemoMenuM1"
#define DEMO_MENU_LABEL "DemoMenuLabel"
#define DEMO_CMD_NAME   "demo.hello"
#define DEMO_BTN_NAME   "DemoHelloBtn"
#define DEMO_BTN_LABEL  "DemoBtnLabel"
#define DEMO_BTN_HELP   "DemoBtnHelp"
#define DEMO_MSG_FILE   "demo_messages.txt"
#define DEMO_HELLO_MSG  L"Hello! demo plugin invoked successfully (M1)."
#endif

#endif /* DEMO_IDS_H */
