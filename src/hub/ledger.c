#include "ledger.h"
#include "registry.h"
#include "hub_log.h"

#include <string.h>

#define TAG "LEDG"
#define LEDGER_MAX 64

typedef struct {
    int  used;
    char id[64];
    char kind[24];
    int  handle;
    LedgerReleaseFn fn;
    void* ctx;
} LedgerEntry;

static LedgerEntry s_led[LEDGER_MAX];

int ledger_add(int pidx, const char* kind, int handle,
               LedgerReleaseFn fn, void* ctx)
{
    int i;
    if (pidx < 0 || pidx >= g_n_plugins || !g_plugins[pidx].id[0]) return 0;
    for (i = 0; i < LEDGER_MAX; i++) {
        if (s_led[i].used) continue;
        s_led[i].used = 1;
        strncpy(s_led[i].id, g_plugins[pidx].id, sizeof(s_led[i].id) - 1);
        s_led[i].id[sizeof(s_led[i].id) - 1] = '\0';
        strncpy(s_led[i].kind, kind ? kind : "-", sizeof(s_led[i].kind) - 1);
        s_led[i].kind[sizeof(s_led[i].kind) - 1] = '\0';
        s_led[i].handle = handle;
        s_led[i].fn = fn;
        s_led[i].ctx = ctx;
        return 1;
    }
    hub_log(TAG, "ERR ledger FULL - add(%s,%s) refused (fail-closed)",
            g_plugins[pidx].id, kind ? kind : "-");
    return 0;
}

static LedgerEntry* find_entry(const char* id, const char* kind, int handle)
{
    int i;
    for (i = 0; i < LEDGER_MAX; i++) {
        if (!s_led[i].used) continue;
        if (strcmp(s_led[i].id, id) == 0 && strcmp(s_led[i].kind, kind) == 0 &&
            s_led[i].handle == handle)
            return &s_led[i];
    }
    return NULL;
}

int ledger_release(int pidx, const char* kind, int handle)
{
    LedgerEntry* e;
    if (pidx < 0 || pidx >= g_n_plugins) return 0;
    e = find_entry(g_plugins[pidx].id, kind, handle);
    if (!e) return 0;
    e->used = 0;
    return 1;
}

int ledger_drain(int pidx)
{
    int n = 0, i;
    if (pidx < 0 || pidx >= g_n_plugins) return 0;
    for (i = LEDGER_MAX - 1; i >= 0; i--) {      /* LIFO by insertion order */
        if (!s_led[i].used || strcmp(s_led[i].id, g_plugins[pidx].id) != 0)
            continue;
        if (s_led[i].fn) s_led[i].fn(s_led[i].id, s_led[i].handle, s_led[i].ctx);
        s_led[i].used = 0;
        n++;
    }
    return n;
}

int ledger_count(int pidx)
{
    int n = 0, i;
    if (pidx < 0 || pidx >= g_n_plugins) return 0;
    for (i = 0; i < LEDGER_MAX; i++)
        if (s_led[i].used && strcmp(s_led[i].id, g_plugins[pidx].id) == 0) n++;
    return n;
}

void ledger_drop(int pidx)
{
    int i;
    if (pidx < 0 || pidx >= g_n_plugins) return;
    for (i = 0; i < LEDGER_MAX; i++)
        if (s_led[i].used && strcmp(s_led[i].id, g_plugins[pidx].id) == 0)
            s_led[i].used = 0;
}
