/* =====================================================================
 * creo-hub M2 OFFLINE test: httpd.c (gate O3 core + O8 httpd-layer fuzz).
 * REAL sockets, REAL port, in-process raw client. Hermetic web root under
 * %%TEMP%%. Run modes:
 *   httpd_test.exe                 -> full O3 matrix + O8 fuzz
 *   httpd_test.exe --serve <port>  -> start server, print SERVING <port>,
 *                                     stay up (for external real clients,
 *                                     e.g. node test\httpd_smoke.mjs)
 * Exit code = number of failures (0 = ALL PASS).
 * ===================================================================== */
#include "httpd.h"          /* brings winsock2/ws2tcpip/windows in order */
#include "hub_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail = 0;
static int g_pass = 0;

extern int g_hub_log_quiet;   /* test_stubs.c: suppress server-side log spam */

#define CHECK(cond, name) do { \
    if (cond) { printf("[PASS] %s\n", name); g_pass++; } \
    else      { printf("[FAIL] %s\n", name); g_fail++; } \
} while (0)

#define BASE_PORT 18765          /* inside the 8765..8775 scan shape */
static char g_troot[MAX_PATH];
static char g_resp[80 * 1024];

/* ------------------------------------------------------------ fs helper */

static void mkdir_p(const char* path)
{
    char tmp[MAX_PATH];
    char* p;
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\') { *p = '\0'; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    }
    CreateDirectoryA(tmp, NULL);
}

static void write_file(const char* path, const char* content)
{
    FILE* f = fopen(path, "wb");
    if (f) { fputs(content, f); fclose(f); }
}

static void setup_webroot(const char* marker)
{
    char sub[MAX_PATH], st[MAX_PATH + 32];
    DWORD n = GetTempPathA(MAX_PATH, sub);
    sub[n] = '\0';
    _snprintf(g_troot, sizeof(g_troot), "%screo_hub_httpd", sub);
    mkdir_p(g_troot);
    _snprintf(sub, sizeof(sub), "%s\\web", g_troot);
    mkdir_p(sub);
    _snprintf(sub, sizeof(sub), "%s\\web\\index.html", g_troot);
    _snprintf(g_resp, sizeof(g_resp),
              "<!doctype html><html><head><title>hub</title></head>"
              "<body>panel-shell %s</body></html>", marker);
    write_file(sub, g_resp);
    _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
    remove(st);
    strcpy(g_root, g_troot);
}

/* ------------------------------------------------------- raw client ops */

static SOCKET cli_connect(int port, int timeout_ms)
{
    SOCKET s;
    struct sockaddr_in sa;
    DWORD tv = (DWORD)timeout_ms;
    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
    return s;
}

/* Send request bytes; if halfclose, shutdown(SD_SEND) so the server sees
 * EOF promptly (fuzz/partial cases). Read until EOF/timeout into g_resp.
 * Returns bytes read (>=0) or -1 on connect failure. */
static int exchange(int port, const char* req, int reqlen, int halfclose)
{
    SOCKET s;
    int total = 0;
    char tmp[8192];
    int off = 0;
    s = cli_connect(port, 3000);
    if (s == INVALID_SOCKET) return -1;
    while (off < reqlen) {
        int r = send(s, req + off, reqlen - off, 0);
        if (r <= 0) break;
        off += r;
    }
    if (halfclose) shutdown(s, SD_SEND);
    for (;;) {
        int r = recv(s, tmp, sizeof(tmp), 0);
        if (r <= 0) break;
        if (total + r < (int)sizeof(g_resp)) { memcpy(g_resp + total, tmp, r); total += r; }
        else break;
    }
    g_resp[total] = '\0';
    closesocket(s);
    return total;
}

static int ex(int port, const char* req)          { return exchange(port, req, (int)strlen(req), 0); }
static int exh(int port, const char* req)         { return exchange(port, req, (int)strlen(req), 1); }

static int resp_status(void)
{
    if (strncmp(g_resp, "HTTP/1.1 ", 9) == 0) return atoi(g_resp + 9);
    return -1;
}

static SOCKET bind_dummy(int port)
{
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (bind(s, (struct sockaddr*)&sa, sizeof(sa)) != 0) { closesocket(s); return INVALID_SOCKET; }
    listen(s, 1);
    return s;
}

/* convenience: request with Host fixed to the listening port */
static int get_root(int port)
{
    char req[256];
    _snprintf(req, sizeof(req),
              "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nConnection: close\r\n\r\n", port);
    return ex(port, req);
}

static unsigned long xorshift_state = 424242UL;
static unsigned long xrnd(void)
{
    xorshift_state ^= xorshift_state << 13;
    xorshift_state ^= xorshift_state >> 17;
    xorshift_state ^= xorshift_state << 5;
    return xorshift_state;
}

/* ===================================================================== */

int main(int argc, char** argv)
{
    WSADATA wsa;
    int port = 0, rc, cyc;
    SOCKET dummy;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { printf("WSAStartup fail\n"); return 2; }

    /* ---------------- serve mode (external real clients) ----------------
     * argv: --serve <port> [seconds]   seconds>0 => auto-exit safety net
     * (keeps pipe handles from outliving the runner under piped shells). */
    if (argc >= 3 && strcmp(argv[1], "--serve") == 0) {
        int want = atoi(argv[2]), actual = 0;
        int secs = (argc >= 4) ? atoi(argv[3]) : 0;
        DWORD t0;
        setup_webroot("hub-httpd-smoke-ok");
        if (!httpd_start(want, &actual)) { printf("SERVE-FAIL\n"); return 2; }
        printf("SERVING %d\n", actual);
        fflush(stdout);
        t0 = GetTickCount();
        while (httpd_is_running()) {
            Sleep(500);
            if (secs > 0 && GetTickCount() - t0 > (DWORD)secs * 1000) break;
        }
        httpd_stop();
        return 0;
    }

    printf("=== creo-hub M2 httpd OFFLINE test (gate O3 + O8 layer) ===\n\n");
    g_hub_log_quiet = 1;   /* server-side log spam off; failures print via CHECK */
    setup_webroot("marker-alpha");

    /* ---------------- S1: start + one-pass port scan ---------------- */
    printf("--- S1 start / port scan ---\n");
    dummy = bind_dummy(BASE_PORT);
    CHECK(dummy != INVALID_SOCKET, "S1 dummy occupies base port");
    rc = httpd_start(BASE_PORT, &port);
    CHECK(rc == 1 && port == BASE_PORT + 1, "S1 base busy -> scanned forward ONCE to +1");
    CHECK(httpd_is_running() == 1 && httpd_port() == BASE_PORT + 1, "S1 running getters agree");
    httpd_stop();
    CHECK(httpd_is_running() == 0, "S1 stop clears running");
    closesocket(dummy);
    rc = httpd_start(BASE_PORT, &port);
    CHECK(rc == 1 && port == BASE_PORT, "S1 port free again -> binds preferred");
    CHECK(get_root(port) > 0 && resp_status() == 200, "S1 serves after clean start");

    /* ---------------- S2: static whitelist + hot swap ---------------- */
    printf("--- S2 static (L7) ---\n");
    rc = get_root(port);
    CHECK(rc > 0 && resp_status() == 200, "S2 GET / -> 200");
    CHECK(strstr(g_resp, "text/html") != NULL, "S2 content-type text/html");
    CHECK(strstr(g_resp, "marker-alpha") != NULL, "S2 body is the web/index.html file");
    CHECK(strstr(g_resp, "Connection: close") != NULL, "S2 Connection: close present");
    CHECK(strstr(g_resp, "Access-Control") == NULL, "L5 no Access-Control headers");
    CHECK(strstr(g_resp, "Set-Cookie") == NULL, "L5 no Set-Cookie header");
    rc = ex(port, "GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 200, "S2 GET /index.html (Host w/o port, case-mixed host) -> 200");
    rc = ex(port, "GET /index.html?a=b&c=d HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 200, "S2 query string stripped for static match");
    rc = ex(port, "GET /nope.html HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 404, "S2 non-whitelisted file -> 404");
    rc = ex(port, "GET /../hub_state.json HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "L7 '..' traversal -> 400");
    rc = ex(port, "GET /..\\windows\\win.ini HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "L7 backslash traversal -> 400");
    rc = ex(port, "GET /%2e%2e/secret HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 404, "L7 encoded traversal never decoded -> 404 (not served)");
    /* hot swap (DoD 9 offline proof): edit file, next GET serves new bytes */
    {
        char sub[MAX_PATH];
        _snprintf(sub, sizeof(sub), "%s\\web\\index.html", g_troot);
        write_file(sub, "<html><body>panel-shell marker-BETA-hotswap</body></html>");
        rc = get_root(port);
        CHECK(rc > 0 && resp_status() == 200 && strstr(g_resp, "marker-BETA-hotswap") != NULL,
              "DoD9 hot swap: edited file served fresh (no cache)");
    }

    /* ---------------- S3: verbs ---------------- */
    printf("--- S3 verbs (L8 face) ---\n");
    rc = ex(port, "OPTIONS / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 405, "S3 OPTIONS -> 405");
    CHECK(strstr(g_resp, "Access-Control") == NULL, "O9 preflight carries NO Allow-Origin/Credentials/Headers");
    rc = ex(port, "PUT / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 405, "S3 PUT -> 405");
    rc = ex(port, "DELETE /api/v1/plugins HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 405, "S3 DELETE on api path -> 405 (fixed verbs only)");
    rc = ex(port, "POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 405, "S3 POST on static path -> 405");
    rc = ex(port, "GET /api/v1/plugins HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 404 && strstr(g_resp, "api not available") != NULL,
          "S3 api route without handler -> 404 json placeholder");

    /* ---------------- S4: malformed / strict parsing / Host L2 ---------------- */
    printf("--- S4 parse strictness + L2 ---\n");
    rc = exh(port, "GARBAGE\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "S4 garbage request -> 400");
    rc = exh(port, "GET /\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "S4 two-token request line -> 400");
    rc = exh(port, "GET / HTTP/9.9\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "S4 unknown HTTP version -> 400");
    rc = exh(port, "GET / HTTP/1.1\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "S4 missing Host -> 400");
    rc = exh(port, "GET / HTTP/1.1\r\nHost: evil.com\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 403, "L2 Host evil.com -> 403");
    {
        char req[256];
        _snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", port + 999);
        rc = exh(port, req);
        CHECK(rc > 0 && resp_status() == 403, "L2 Host with WRONG port -> 403");
        _snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: 127.0.0.1:%d\r\n\r\n", port);
        rc = exh(port, req);
        CHECK(rc > 0 && resp_status() == 200, "L2 Host with correct port -> 200");
    }
    rc = exh(port, "GET / HTTP/1.1\r\nHost: localhost\r\nHost: other\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "S4 duplicate Host -> 400 (smuggling hardening)");
    rc = exh(port, "POST /api/v1/x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n0");
    CHECK(rc > 0 && resp_status() == 400, "S4 duplicate Content-Length -> 400");
    rc = exh(port, "POST /api/v1/x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 12abc\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "S4 non-digit Content-Length -> 400");
    rc = exh(port, "POST /api/v1/x HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1,2\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 400, "S4 list-style Content-Length -> 400");
    rc = exh(port, "GET / HTTP/1.1\nHost: localhost\n\n");
    CHECK(rc >= 0 && (rc == 0 || resp_status() == 400), "S4 bare-LF line ends never complete -> 400/close");
    {   /* NUL byte inside header block */
        char req[128];
        int n = _snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: localhost\r\nX: a");
        req[n] = '\0'; req[n + 1] = 'b';
        memcpy(req + n + 2, "\r\n\r\n", 4);
        rc = exchange(port, req, n + 6, 1);
        CHECK(rc > 0 && resp_status() == 400, "S4 NUL byte in headers -> 400");
    }
    {   /* header block over 8KB cap */
        static char big[12000];
        int n, i;
        n = sprintf(big, "GET /");
        for (i = 0; i < 9500; i++) big[n++] = 'a';
        n += sprintf(big + n, " HTTP/1.1\r\nHost: localhost\r\n\r\n");
        rc = exh(port, big);
        CHECK(rc > 0 && resp_status() == 400, "L6 header block over 8KB cap -> 400");
    }

    /* ---------------- S5: body caps + read ---------------- */
    printf("--- S5 body (L6) ---\n");
    rc = exh(port, "POST /api/v1/mount HTTP/1.1\r\nHost: localhost\r\nContent-Length: 70000\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 413, "L6 Content-Length 70000 > 64KB cap -> 413 (body NOT read)");
    rc = exh(port, "POST /api/v1/mount HTTP/1.1\r\nHost: localhost\r\nContent-Length: 12345678901\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 413, "L6 absurd Content-Length (>9 digits) -> 413");
    rc = ex(port, "POST /api/v1/mount HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\n0123456789");
    CHECK(rc > 0 && resp_status() == 404, "S5 exact body consumed, routed (404 placeholder, pre-A5)");
    CHECK(get_root(port) > 0 && resp_status() == 200, "S5 worker healthy right after body read");
    rc = exchange(port, "POST /api/v1/mount HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n\r\n01234", 73, 1);
    CHECK(rc > 0 && resp_status() == 408, "S5 short body + EOF -> 408 body timeout");
    rc = ex(port, "POST /api/v1/mount HTTP/1.1\r\nHost: localhost\r\n\r\n");
    CHECK(rc > 0 && resp_status() == 404, "S5 POST without CL reaches router (411 policy is web_api's, A-5)");

    /* ---------------- S6: slow client deadline ---------------- */
    printf("--- S6 slow client (takes ~6s) ---\n");
    {
        SOCKET s = cli_connect(port, 9000);
        char part[128];
        int n = _snprintf(part, sizeof(part),
                          "GET / HTTP/1.1\r\nHost: localhost:%d\r\nX-Pad: ", port);
        char acc[4096]; int total = 0;
        send(s, part, n, 0);
        Sleep(6500);                 /* server deadline (5s) must fire */
        for (;;) { int r = recv(s, acc + total, sizeof(acc) - 1 - total, 0); if (r <= 0) break; total += r; }
        acc[total] = '\0';
        memcpy(g_resp, acc, total + 1);
        closesocket(s);
        CHECK(total == 0 || resp_status() == 408, "L6 stalled header send -> 408 or clean close (no hang)");
    }

    /* ---------------- S7: stop/start cycles (E9 offline precursor) ---------------- */
    printf("--- S7 lifecycle x3 ---\n");
    for (cyc = 0; cyc < 3; cyc++) {
        char lbl[64];
        _snprintf(lbl, sizeof(lbl), "S7 cycle %d: stop -> port released", cyc + 1);
        httpd_stop();
        CHECK(httpd_is_running() == 0, lbl);
        _snprintf(lbl, sizeof(lbl), "S7 cycle %d: restart binds SAME port", cyc + 1);
        rc = httpd_start(BASE_PORT, &port);
        CHECK(rc == 1 && port == BASE_PORT, lbl);
        _snprintf(lbl, sizeof(lbl), "S7 cycle %d: serves after restart", cyc + 1);
        CHECK(get_root(port) > 0 && resp_status() == 200, lbl);
    }

    /* ---------------- S8: overlapping connections (serial worker) ---------------- */
    printf("--- S8 overlap ---\n");
    {
        char req[256];
        SOCKET a, b;
        int ra, rb;
        char ba[4096], bb[4096];
        int ta = 0, tb = 0;
        _snprintf(req, sizeof(req),
                  "GET /index.html HTTP/1.1\r\nHost: localhost:%d\r\n\r\n", port);
        a = cli_connect(port, 6000);
        b = cli_connect(port, 6000);
        CHECK(a != INVALID_SOCKET && b != INVALID_SOCKET, "S8 two simultaneous connections accepted (backlog)");
        send(a, req, (int)strlen(req), 0);
        send(b, req, (int)strlen(req), 0);
        for (;;) { int r = recv(a, ba + ta, sizeof(ba) - 1 - ta, 0); if (r <= 0) break; ta += r; }
        for (;;) { int r = recv(b, bb + tb, sizeof(bb) - 1 - tb, 0); if (r <= 0) break; tb += r; }
        ba[ta] = '\0'; bb[tb] = '\0';
        ra = (ta > 12) ? atoi(ba + 9) : -1;
        rb = (tb > 12) ? atoi(bb + 9) : -1;
        CHECK(ra == 200 && rb == 200, "S8 both overlapping connections get 200 (serial processing)");
        closesocket(a); closesocket(b);
    }

    /* ---------------- S9: O8 httpd-layer fuzz (2000 rounds) ---------------- */
    printf("--- S9 O8 fuzz x2000 (fixed seed) ---\n");
    {
        int it, bad = 0, hangs = 0, dead = 0;
        for (it = 0; it < 2000; it++) {
            char buf[3072];
            int len = 0, got;
            unsigned long mode = xrnd() % 8;
            switch (mode) {
            case 0:   /* pure random bytes */
                len = (int)(xrnd() % 400) + 1;
                { int i; for (i = 0; i < len; i++) buf[i] = (char)(xrnd() & 0xFF); }
                break;
            case 1:   /* valid line + junk headers */
                len = sprintf(buf, "GET / HTTP/1.1\r\n");
                { int i, hn = (int)(xrnd() % 5) + 1;
                  for (i = 0; i < hn; i++) len += sprintf(buf + len, "H%d: %lu\r\n", i, xrnd()); }
                len += sprintf(buf + len, "\r\n");
                break;
            case 2:   /* random printable junk request line */
                len = (int)(xrnd() % 120) + 10;
                { int i; for (i = 0; i < len; i++) buf[i] = (char)(0x21 + xrnd() % 0x5E); buf[len++] = '\r'; buf[len++] = '\n'; buf[len++] = '\r'; buf[len++] = '\n'; }
                break;
            case 3:   /* huge Content-Length */
                len = sprintf(buf, "POST /api/v1/mount HTTP/1.1\r\nHost: localhost:%d\r\nContent-Length: %lu\r\n\r\n", port, 100000 + xrnd() % 900000);
                break;
            case 4: { /* truncated valid request */
                int full = sprintf(buf, "GET /index.html HTTP/1.1\r\nHost: localhost:%d\r\nAccept: */*\r\n\r\n", port);
                len = (int)(xrnd() % (unsigned)full);
                break; }
            case 5:   /* bare-LF endings */
                len = sprintf(buf, "GET / HTTP/1.1\nHost: localhost:%d\n\n", port);
                break;
            case 6:   /* NUL bytes embedded */
                len = sprintf(buf, "GET / HTTP/1.1\r\nHost: localhost:%d\r\nX: ", port);
                buf[len++] = '\0';
                buf[len++] = 'Z';
                memcpy(buf + len, "\r\n\r\n", 4);
                len += 4;
                break;
            default:  /* overlong URL */
                len = sprintf(buf, "GET /");
                { int i, ul = (int)(xrnd() % 2000) + 100;
                  for (i = 0; i < ul && len < 2500; i++) buf[len++] = (char)('a' + xrnd() % 26); }
                len += sprintf(buf + len, " HTTP/1.1\r\nHost: localhost:%d\r\n\r\n", port);
                break;
            }
            got = exchange(port, buf, len, 1);
            if (got == -1) { dead++; continue; }          /* connect refused = server died */
            if (got == 0) continue;                        /* clean close = acceptable */
            {
                int st = resp_status();
                if (st >= 400 && st < 500) continue;       /* any 4xx = acceptable */
                if (st == 200 && (mode == 1 || mode == 4)) continue; /* occasionally valid */
                bad++;
                printf("  fuzz[%d] mode=%lu unexpected status=%d\n", it, mode, st);
            }
        }
        CHECK(dead == 0, "O8 server never died during fuzz (connect always ok)");
        CHECK(bad == 0, "O8 every fuzz response was 4xx/close (no 5xx, no garbage)");
        CHECK(hangs == 0, "O8 no fuzz case hung the client (half-close EOF discipline)");
        CHECK(get_root(port) > 0 && resp_status() == 200, "O8 normal request STILL 200 after 2000 fuzz rounds");
        CHECK(httpd_is_running() == 1, "O8 worker thread alive after fuzz");
    }

    httpd_stop();
    {
        char st[MAX_PATH + 32];
        _snprintf(st, sizeof(st), "%s\\hub_state.json", g_troot);
        remove(st);
    }
    WSACleanup();
    printf("\n=== RESULT: %d pass, %d fail -> %s ===\n",
           g_pass, g_fail, g_fail ? "FAIL" : "ALL PASS");
    return g_fail ? 1 : 0;
}
