/* =====================================================================
 * creo-hub M2: hub_token.c - G4 session token (see hub_token.h).
 * Primary entropy: CryptGenRandom (advapi32, Win7-safe). Fallback (only
 * if CryptoAPI is unavailable): QPC ^ tick ^ pid mixed splitmix64 -
 * acceptable because the guarded face is loopback-only; the fallback is
 * logged loudly so it can never pass silently.
 * ===================================================================== */
#include "hub_token.h"
#include "hub_log.h"

#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>

#define TAG "TOKEN"

static char g_tok[33] = "";

static int fill_random(unsigned char* buf, int n)
{
    HCRYPTPROV h = 0;
    if (CryptAcquireContextA(&h, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        BOOL ok = CryptGenRandom(h, (DWORD)n, buf);
        CryptReleaseContext(h, 0);
        if (ok) return 1;
    }
    {   /* fallback mixer */
        LARGE_INTEGER qpc;
        unsigned long long x;
        int i;
        QueryPerformanceCounter(&qpc);
        x = ((unsigned long long)qpc.QuadPart << 20)
            ^ ((unsigned long long)GetTickCount() * 2654435761ULL)
            ^ ((unsigned long long)GetCurrentProcessId() * 40503ULL);
        for (i = 0; i < n; i++) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            buf[i] = (unsigned char)(x >> 24);
        }
        hub_log(TAG, "WARNING: CryptGenRandom unavailable - fallback entropy used");
        return 1;
    }
}

int hub_token_generate(void)
{
    unsigned char b[16];
    int i;
    if (!fill_random(b, 16)) return 0;
    for (i = 0; i < 16; i++) sprintf(g_tok + 2 * i, "%02x", b[i]);
    g_tok[32] = '\0';
    hub_log(TAG, "session token generated (128-bit; value never logged)");
    return 1;
}

const char* hub_token_value(void)
{
    return g_tok;
}

int hub_token_write_file(const char* root)
{
    char path[MAX_PATH + 32];
    FILE* f;
    if (!g_tok[0]) {
        hub_log(TAG, "write skipped: no token generated yet");
        return 0;
    }
    _snprintf(path, sizeof(path), "%s\\hub_token.txt", root);
    f = fopen(path, "wb");
    if (!f) {
        hub_log(TAG, "cannot write %s (err=%lu)", path, GetLastError());
        return 0;
    }
    fputs(g_tok, f);
    fputs("\r\n", f);
    fclose(f);
    hub_log(TAG, "G4 token file written: %s (per-start rotation; ACL inherited = same user)", path);
    return 1;
}

int hub_token_delete_file(const char* root)
{
    char path[MAX_PATH + 32];
    _snprintf(path, sizeof(path), "%s\\hub_token.txt", root);
    if (DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND) {
        hub_log(TAG, "token file removed (rotation hygiene)");
        return 1;
    }
    hub_log(TAG, "token file delete failed (%lu)", GetLastError());
    return 0;
}
