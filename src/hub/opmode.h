#ifndef CREO_HUB_OPMODE_H
#define CREO_HUB_OPMODE_H

/* =====================================================================
 * creo-hub M3e Q1: operator mode menu (user-ratified 2026-10-13:
 * "增加选择路径，风险由用户承担"). Three modes, human-armed from the
 * panel, memory-only (hub restart -> M0), every auto-pass audited:
 *   M0 OPMODE_STANDARD   : confirm+destructive need panel approval (default,
 *                          identical to pre-M3e behaviour);
 *   M1 OPMODE_TRUST      : confirm auto-passes (TRUST session), destructive
 *                          STILL panel-required (highest risk keeps a human);
 *   M2 OPMODE_UNATTENDED : timed window (5/10/30 min) in which confirm AND
 *                          destructive auto-pass; expiry -> M0 + audit.
 * Threat-model note (guide §8.4.4): the arm endpoint sits behind the same
 * loopback token as /confirm approve - same-user trust boundary, unchanged.
 * ===================================================================== */

#define OPMODE_STANDARD   0
#define OPMODE_TRUST      1
#define OPMODE_UNATTENDED 2

void opmode_init(void);

/* effective mode right now (M2 expiry folded in; expiry transition logs). */
int  opmode_current(void);

/* arm/disarm. mode=0 disarms. mode=2 requires window_ms>0 && ack==1
 * (panel checkbox "I understand destructive executes unapproved").
 * returns 0 ok, -1 bad args. */
int  opmode_set(int mode, unsigned long long window_ms, int ack);

/* remaining armed-window ms (0 when not M2); out_remain may be NULL. */
int  opmode_get(int* out_mode, unsigned long long* out_remain_ms);

/* 1 = this risk auto-passes the nonce gate under the current mode. */
int  opmode_allows(const char* risk);

#endif /* CREO_HUB_OPMODE_H */
