/* creo-hub M3b b3: P2 controlled-surface ledger (docs\14 3.4 / docs\23 3.7).
 * Per-plugin LIFO cleanup chain of hub-side registrations. v1 entry points:
 *   - "nonce"  : confirmation-table slot ownership (nonce.c issue/release)
 *   - "mount"  : mount-session anchor (registry_mount success)
 * Entries are keyed by plugin ID (survives rescan reindex). Unmount order:
 * ledger_drain(idx) LIFO BEFORE user_terminate/DllUnload - the hub-side
 * analogue of the L-Unload accumulator rule. Residual entries after
 * drain are a hub bug -> ERR audit (E13 assertion executed). Listener kinds
 * arrive with M4 async; the API is ready for them. */
#ifndef LEDGER_H
#define LEDGER_H

typedef void (*LedgerReleaseFn)(const char* plugin_id, int handle, void* ctx);

/* Bookkeeping add. kind is a short tag ("nonce", "mount", ...). fn may be
 * NULL (pure bookkeeping; drain then just clears the entry). Returns 1 ok,
 * 0 when the table is full (fail-closed: caller must treat as error). */
int  ledger_add(int pidx, const char* kind, int handle,
                LedgerReleaseFn fn, void* ctx);

/* Exact-match release (kind+handle). Returns 1 when an entry was freed. */
int  ledger_release(int pidx, const char* kind, int handle);

/* LIFO release of EVERY entry of pidx (calls fns). Returns released count. */
int  ledger_drain(int pidx);

/* Live entry count of pidx (0 = clean). */
int  ledger_count(int pidx);

/* Drop bookkeeping of pidx without calling fns (record teardown). */
void ledger_drop(int pidx);

#endif /* LEDGER_H */
