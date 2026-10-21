/* creo-hub M2 (E6, docs\09 2.2/2.3): cross-thread marshaling to the Creo
 * main thread. Pure Win32 (message-only window), fully offline-testable.
 * The httpd worker NEVER touches PTK/registry mutators directly: web_api
 * enqueues a G1 envelope and calls the registered wakeup (hub_marsh_post);
 * the message window's WndProc - which always runs on the thread that
 * created it (Creo main thread, where user_initialize runs) - pumps the
 * executor callback until the queue is dry. A 500ms ProUITimer on the Creo
 * side re-pumps as a safety net if PostMessage is ever lost (X3 fallback). */
#ifndef HUB_MARSH_H
#define HUB_MARSH_H

#include <windows.h>

/* Executor pump callback: run ONE unit of pending work; return 1 if work
 * was done, 0 when dry. hub_main passes a web_api_run_pending wrapper. */
typedef int (*hub_marsh_pump_fn)(void);

/* Create the message-only pump window ON THE CALLING THREAD (must be the
 * Creo main thread in production). Returns 1 on success. */
int  hub_marsh_init(hub_marsh_pump_fn fn);

/* Destroy the window (safe from any thread: cross-thread shutdown is
 * marshaled to the owner via SendMessage). Unregisters the class. */
void hub_marsh_shutdown(void);

/* Request one pump pass on the owner thread (PostMessage, non-blocking,
 * any thread). Returns 1 when posted, 0 on failure (fallback timer will
 * still pump within 500ms in production). */
int  hub_marsh_post(void);

/* 1 when the calling thread owns the pump window. */
int  hub_marsh_is_main_thread(void);

#endif /* HUB_MARSH_H */
