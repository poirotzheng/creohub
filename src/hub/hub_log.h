/* creo-hub M1: shared logging / error-name / popup utilities */
#ifndef HUB_LOG_H
#define HUB_LOG_H

#include <windows.h>
#include <ProToolkit.h>
#include <ProUIMessage.h>

extern char g_root[MAX_PATH];
extern char g_dll_dir[MAX_PATH];

/* b4 R1: primary log rotation cap (bytes); <=0 disables. Single generation:
 * hub_m1.log -> hub_m1.log.1 (replaced). Tests may shrink it. */
extern long g_hub_log_max_bytes;

void hub_detect_root(void);
void hub_log(const char* tag, const char* fmt, ...);
const char* hub_errname(ProError e);
void hub_popup(int is_err, const wchar_t* title, const wchar_t* msg);
void hub_popup_result(int is_err, const char* tag, ProError ret,
                      ProError user_err, const wchar_t* user_msg);

#endif /* HUB_LOG_H */
