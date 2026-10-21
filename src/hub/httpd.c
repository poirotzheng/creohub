/* =====================================================================
 * creo-hub M2: httpd.c - minimal HTTP/1.1 server (docs\09 2.1/2.7).
 * One accept-loop worker thread, serial connection processing,
 * Connection: close, no keep-alive, no chunked encoding.
 * Security layers implemented HERE (others live in web_api/panel):
 *   L1 loopback bind, compile-time constant (HTTPD_BIND_ADDR)
 *      + per-connection getpeername defense-in-depth
 *   L2 Host header exact whitelist {127.0.0.1, localhost} (+ actual port)
 *   L5 no CORS / no cookie headers ever emitted (by construction)
 *   L6 caps (hdr 8KB / body 64KB) + SO_RCVTIMEO/SO_SNDTIMEO + total
 *      per-request deadline (slowloris-resistant)
 *   L7 static whitelist {/ -> index.html}, path NOT URL-decoded,
 *      ".." and "\" rejected with 400
 *   L9 start/stop lifecycle owned by hub_main (M2b), logged.
 *   L10 Origin whitelist (b4, docs\23 3.8 as ruled 2026-10-10): absent
 *      Origin passes (curl/node/bridge/IE); present Origin must be
 *      exactly http://127.0.0.1[:port] or http://localhost[:port] with
 *      the actual listening port, else 403. Duplicate Origin -> 400.
 * Win7 baseline: inet_pton (Vista+) only, no Win8+ APIs.
 * ===================================================================== */
#include "httpd.h"
#include "hub_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define TAG "HTTP"

static SOCKET          s_listen  = INVALID_SOCKET;
static HANDLE          s_thread  = NULL;
static volatile LONG   s_running = 0;
static int             s_port    = 0;
static httpd_route_fn  s_api     = NULL;

/* ------------------------------------------------------------ utilities */

static const char* status_reason(int s)
{
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "Error";
    }
}

static void send_all(SOCKET s, const char* data, int len)
{
    int off = 0;
    while (off < len) {
        int r = send(s, data + off, len - off, 0);
        if (r <= 0) return;   /* peer gone / timeout: drop silently */
        off += r;
    }
}

static char* httpd_read_file_alloc(const char* path, long* out_len)
{
    FILE* f = fopen(path, "rb");
    long sz;
    char* buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    if (out_len) *out_len = sz;
    return buf;
}

/* ------------------------------------------------------------ responses */

void httpd_respond(HttpdConn* conn, int status, const char* content_type,
                   const char* body, int body_len)
{
    char head[512];
    int hn;
    if (body_len < 0) body_len = body ? (int)strlen(body) : 0;
    /* L5: this template is the ONLY header set the server emits. It
     * deliberately contains no Access-Control-Allow-* and no Set-Cookie. */
    hn = _snprintf(head, sizeof(head),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: %s\r\n"
                   "Content-Length: %d\r\n"
                   "Connection: close\r\n"
                   "Cache-Control: no-store\r\n"
                   "\r\n",
                   status, status_reason(status),
                   content_type ? content_type : "text/plain; charset=utf-8",
                   body_len);
    if (hn <= 0) return;
    send_all(conn->sock, head, hn);
    if (body && body_len > 0) send_all(conn->sock, body, body_len);
}

void httpd_respond_json(HttpdConn* conn, int status, const char* json_text)
{
    httpd_respond(conn, status, "application/json; charset=utf-8", json_text, -1);
}

/* ------------------------------------------------------------ header RX */

/* Returns:
 *   1  ok  (buffer NUL-terminated after CRLFCRLF, *out_len = header bytes)
 *   0  peer closed having sent nothing
 *  -2  timeout / total deadline exceeded (slow or stalled client)
 *  -3  peer closed mid-headers (partial request)
 *  -4  header block exceeds cap */
static int recv_headers(SOCKET s, char* buf, int cap, int* out_len)
{
    DWORD start = GetTickCount();
    int n = 0;
    while (n < cap - 1) {
        char c;
        int r;
        if (GetTickCount() - start > HTTPD_IO_TIMEOUT_MS) return -2;
        r = recv(s, &c, 1, 0);
        if (r == 0) return (n > 0) ? -3 : 0;
        if (r < 0) {
            if (WSAGetLastError() == WSAETIMEDOUT) return -2;
            return -3;
        }
        buf[n++] = c;
        if (n >= 4 && memcmp(buf + n - 4, "\r\n\r\n", 4) == 0) {
            buf[n] = '\0';
            *out_len = n;
            return 1;
        }
    }
    return -4;
}

/* Read exactly want bytes (body) with total deadline. 1 ok, 0 fail. */
static int recv_exact(SOCKET s, char* buf, int want)
{
    DWORD start = GetTickCount();
    int got = 0;
    while (got < want) {
        int r;
        if (GetTickCount() - start > HTTPD_IO_TIMEOUT_MS) return 0;
        r = recv(s, buf + got, want - got, 0);
        if (r <= 0) return 0;
        got += r;
    }
    return 1;
}

/* ------------------------------------------------------------ parsing */

static int is_token_char(unsigned char c)
{
    return c > 0x20 && c < 0x7F;   /* RFC-ish token subset, no CTL/SEP */
}

/* Validate Host per L2: host part must be exactly "127.0.0.1" or
 * "localhost" (case-insensitive); optional ":port" must equal the actual
 * listening port. Returns 1 ok, 0 reject (403). */
static int host_allowed(const char* host_hdr, int actual_port)
{
    char buf[256];
    char* colon;
    char* p;
    strncpy(buf, host_hdr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    colon = strchr(buf, ':');
    if (colon) {
        *colon = '\0';
        if (!colon[1]) return 0;
        for (p = colon + 1; *p; p++)
            if (!isdigit((unsigned char)*p)) return 0;
        if (atoi(colon + 1) != actual_port) return 0;
    }
    for (p = buf; *p; p++) *p = (char)tolower((unsigned char)*p);
    return (strcmp(buf, "127.0.0.1") == 0 || strcmp(buf, "localhost") == 0);
}

/* b4 L10 (docs\23 3.8 as ruled 2026-10-10, docs\25): Origin whitelist.
 * Absent Origin -> allowed (curl/node/bridge/IE panel never send one).
 * Present Origin -> must be EXACTLY http://127.0.0.1[:port] or
 * http://localhost[:port], port (when present) == actual listening port.
 * Everything else (including "null", https, any external domain) -> 403.
 * Rationale for whitelist instead of blanket ban: the Fetch spec attaches
 * Origin to ALL POSTs (same-origin included) - a blanket ban would kill
 * every panel button in Chrome/Edge while adding zero security: browsers
 * forge-proof the header, so an attacker page's Origin is always its own
 * (external) domain under both policies. */
static int origin_allowed(const char* origin, int actual_port)
{
    const char* p = origin;
    char host[256];
    char* colon;
    char* q;
    int i = 0;
    if (_strnicmp(p, "http://", 7) != 0) return 0;
    p += 7;
    while (*p && *p != ':' && *p != '/' && i < (int)sizeof(host) - 1)
        host[i++] = *p++;
    host[i] = '\0';
    if (*p == '/') return 0;                 /* Origin carries no path */
    if (*p == ':') {
        const char* d = p + 1;
        if (!*d) return 0;
        while (*d) {
            if (!isdigit((unsigned char)*d)) return 0;
            d++;
        }
        if (atoi(d ? p + 1 : "") != actual_port) return 0;
    } else if (*p) {
        return 0;                            /* trailing garbage */
    }
    for (q = host; *q; q++) *q = (char)tolower((unsigned char)*q);
    return (strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0);
}

/* Parse request line + headers in-place (destructive).
 * Returns HTTP status to answer with on rejection, 200 on success.
 * Strictness rules (smuggling hardening): only CRLF line ends, no control
 * chars inside header text, no duplicate Host/Content-Length, CL is a
 * single all-digit token, exactly 3 request-line tokens, version HTTP/1.x. */
static int parse_request(HttpdRequest* q, char* hdr, int hdrlen, int actual_port)
{
    char* line_end;
    char* sp1;
    char* sp2;
    char* ver;
    char* cur;
    int i, host_seen = 0, cl_seen = 0;

    memset(q, 0, sizeof(*q));
    q->content_length = -1;

    for (i = 0; i < hdrlen; i++) {
        unsigned char c = (unsigned char)hdr[i];
        if (c < 0x20 && c != '\r' && c != '\n') return 400;
    }

    line_end = strstr(hdr, "\r\n");
    if (!line_end) return 400;
    *line_end = '\0';

    sp1 = strchr(hdr, ' ');
    if (!sp1) return 400;
    *sp1 = '\0';
    sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) return 400;
    *sp2 = '\0';
    ver = sp2 + 1;
    if (strchr(ver, ' ')) return 400;

    if (strlen(hdr) >= sizeof(q->method)) return 400;
    if (strlen(sp1 + 1) >= sizeof(q->raw_target)) return 400;
    for (i = 0; hdr[i]; i++)
        if (!is_token_char((unsigned char)hdr[i])) return 400;

    if (strncmp(ver, "HTTP/1.", 7) != 0) return 400;
    if (!isdigit((unsigned char)ver[7]) || ver[8] != '\0') return 400;
    q->http_minor = ver[7] - '0';

    strcpy(q->method, hdr);
    strcpy(q->raw_target, sp1 + 1);

    /* split target into path + query; path stays URL-ENCODED (L7: the
     * static whitelist matches literals, so %2e%2e can never decode to "..") */
    {
        char* qm = strchr(q->raw_target, '?');
        if (qm) {
            *qm = '\0';
            strncpy(q->query, qm + 1, sizeof(q->query) - 1);
        }
        strncpy(q->path, q->raw_target, sizeof(q->path) - 1);
    }
    if (q->path[0] != '/') return 400;

    cur = line_end + 2;
    while (cur < hdr + hdrlen && *cur) {
        char* key;
        char* val;
        char* colon;
        if (cur[0] == '\r' && cur[1] == '\n') break;   /* blank line ends headers */
        line_end = strstr(cur, "\r\n");
        if (!line_end) return 400;
        *line_end = '\0';
        colon = strchr(cur, ':');
        if (!colon) return 400;
        *colon = '\0';
        key = cur;
        val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;
        if (!*key) return 400;
        {   /* lowercase key in place for compare */
            char* p;
            for (p = key; *p; p++) *p = (char)tolower((unsigned char)*p);
        }
        if (!strcmp(key, "host")) {
            if (host_seen) return 400;
            host_seen = 1;
            strncpy(q->host, val, sizeof(q->host) - 1);
        } else if (!strcmp(key, "content-length")) {
            char* p;
            if (cl_seen) return 400;
            cl_seen = 1;
            if (!*val) return 400;
            for (p = val; *p; p++)
                if (!isdigit((unsigned char)*p)) return 400;
            if (strlen(val) > 9) return 413;
            q->content_length = atoi(val);
        } else if (!strcmp(key, "content-type")) {
            strncpy(q->content_type, val, sizeof(q->content_type) - 1);
        } else if (!strcmp(key, "x-hub-token")) {
            strncpy(q->token, val, sizeof(q->token) - 1);
        } else if (!strcmp(key, "origin")) {
            if (q->origin[0]) return 400;            /* duplicate Origin */
            strncpy(q->origin, val, sizeof(q->origin) - 1);
        }
        cur = line_end + 2;
    }

    if (!host_seen) return 400;              /* Host required (even HTTP/1.0) */
    if (!host_allowed(q->host, actual_port)) return 403;
    /* b4 L10: Origin whitelist (absent -> pass; present -> loopback only) */
    if (q->origin[0] && !origin_allowed(q->origin, actual_port)) return 403;
    return 200;
}

/* ------------------------------------------------------------ static L7 */

static void serve_static(HttpdConn* c, const HttpdRequest* q)
{
    char fp[MAX_PATH + 64];
    char* txt;
    long len = 0;

    if (strcmp(q->path, "/") != 0 && strcmp(q->path, "/index.html") != 0) {
        /* traversal patterns get an explicit 400 + audit line */
        if (strstr(q->path, "..") || strchr(q->path, '\\')) {
            hub_log(TAG, "REJECT traversal attempt target='%s'", q->raw_target);
            httpd_respond(c, 400, "text/plain; charset=utf-8", "bad path", -1);
            return;
        }
        httpd_respond(c, 404, "text/plain; charset=utf-8", "not found", -1);
        return;
    }
    _snprintf(fp, sizeof(fp), "%s\\web\\index.html", g_root);
    txt = httpd_read_file_alloc(fp, &len);
    if (!txt) {
        httpd_respond(c, 404, "text/plain; charset=utf-8", "panel file missing", -1);
        return;
    }
    /* fresh read per request = hot-swap capability (DoD 9, offline proof) */
    httpd_respond(c, 200, "text/html; charset=utf-8", txt, (int)len);
    free(txt);
}

/* ------------------------------------------------------------ connection */

static void handle_connection(SOCKET cs)
{
    HttpdConn conn;
    HttpdRequest q;
    char hdr[HTTPD_HDR_MAX + 8];
    int hdrlen = 0;
    int pr;
    DWORD tv = HTTPD_IO_TIMEOUT_MS;
    struct sockaddr_in pa;
    struct in_addr lb;
    int plen = (int)sizeof(pa);

    conn.sock = cs;
    setsockopt(cs, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(cs, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

    /* L1 defense-in-depth: even though we bind loopback-only, verify the
     * accepted peer address is the loopback literal. */
    memset(&pa, 0, sizeof(pa));
    inet_pton(AF_INET, HTTPD_BIND_ADDR, &lb);
    if (getpeername(cs, (struct sockaddr*)&pa, &plen) == 0 &&
        pa.sin_addr.s_addr != lb.s_addr) {
        hub_log(TAG, "REJECT non-loopback peer");
        httpd_respond(&conn, 403, "text/plain; charset=utf-8", "forbidden", -1);
        return;
    }

    pr = recv_headers(cs, hdr, HTTPD_HDR_MAX, &hdrlen);
    if (pr == 0) return;                       /* silent connect+close */
    if (pr == -2) {
        hub_log(TAG, "request timeout (slow/stalled client)");
        httpd_respond(&conn, 408, "text/plain; charset=utf-8", "request timeout", -1);
        return;
    }
    if (pr == -4) {
        hub_log(TAG, "REJECT header block over %d bytes", HTTPD_HDR_MAX);
        httpd_respond(&conn, 400, "text/plain; charset=utf-8", "headers too large", -1);
        return;
    }
    if (pr < 0) {                              /* partial then EOF */
        httpd_respond(&conn, 400, "text/plain; charset=utf-8", "malformed request", -1);
        return;
    }

    pr = parse_request(&q, hdr, hdrlen, s_port);
    if (pr != 200) {
        if (pr == 403) hub_log(TAG, "REJECT Host '%s' (whitelist {127.0.0.1,localhost} + port %d)", q.host, s_port);
        else           hub_log(TAG, "REJECT malformed request (%d)", pr);
        httpd_respond(&conn, pr, "text/plain; charset=utf-8", status_reason(pr), -1);
        return;
    }

    /* body (L6 caps; CL>max rejected WITHOUT reading) */
    if (q.content_length > HTTPD_BODY_MAX) {
        hub_log(TAG, "REJECT body Content-Length %d over cap %d", q.content_length, HTTPD_BODY_MAX);
        httpd_respond(&conn, 413, "text/plain; charset=utf-8", "body too large", -1);
        return;
    }
    if (q.content_length > 0) {
        q.body = (char*)malloc((size_t)q.content_length + 1);
        if (!q.body) {
            httpd_respond(&conn, 500, "text/plain; charset=utf-8", "oom", -1);
            return;
        }
        if (!recv_exact(cs, q.body, q.content_length)) {
            hub_log(TAG, "body truncated (CL=%d) - timeout or short send", q.content_length);
            free(q.body);
            httpd_respond(&conn, 408, "text/plain; charset=utf-8", "body timeout", -1);
            return;
        }
        q.body[q.content_length] = '\0';
        q.body_len = q.content_length;
    }

    /* dispatch: /api/v1/* -> registered handler (A-5); static whitelist;
     * anything else 404/405. Fixed verbs only (L8 at routing face). */
    if (strncmp(q.path, "/api/v1", 7) == 0 &&
        (q.path[7] == '\0' || q.path[7] == '/')) {
        /* L8: api face exposes GET (reads) + POST (actions) ONLY */
        if (strcmp(q.method, "GET") != 0 && strcmp(q.method, "POST") != 0) {
            httpd_respond(&conn, 405, "text/plain; charset=utf-8", "method not allowed", -1);
        } else {
            httpd_route_fn fn = s_api;
            if (fn && fn(&q, &conn)) {
                /* handled */
            } else {
                httpd_respond_json(&conn, 404, "{\"ok\":false,\"error\":\"api not available\"}");
            }
        }
    } else if (strcmp(q.method, "GET") == 0) {
        serve_static(&conn, &q);
    } else {
        httpd_respond(&conn, 405, "text/plain; charset=utf-8", "method not allowed", -1);
    }

    free(q.body);
}

/* ------------------------------------------------------------ lifecycle */

static DWORD WINAPI httpd_worker(LPVOID arg)
{
    (void)arg;
    hub_log(TAG, "worker thread start");
    while (s_running) {
        SOCKET cs = accept(s_listen, NULL, NULL);
        if (cs == INVALID_SOCKET) {
            if (s_running)
                hub_log(TAG, "accept failed (%d) - worker exiting", WSAGetLastError());
            break;   /* stop signal (listener closed) or fatal */
        }
        handle_connection(cs);
        closesocket(cs);
    }
    hub_log(TAG, "worker thread exit");
    return 0;
}

int httpd_start(int preferred_port, int* actual_port_out)
{
    WSADATA wsa;
    struct sockaddr_in sa;
    int port;
    int port_last;
    int bound = 0;

    if (s_thread) {
        hub_log(TAG, "start ignored: already running on port %d", s_port);
        if (actual_port_out) *actual_port_out = s_port;
        return 1;
    }
    /* wiring guard (9/13 field bug): a listener without an API handler serves
     * the static panel but answers every /api/v1/* with an UNLOGGED 404 -
     * scream loudly so the mistake can never hide again. */
    if (!s_api)
        hub_log(TAG, "WARNING: no API handler registered - /api/v1/* will 404 "
                     "(production wiring bug if this is the hub DLL!)");
    if (preferred_port < HTTPD_PORT_FIRST) preferred_port = HTTPD_PORT_FIRST;
    /* one-pass scan window = the documented 11-port span starting at
     * preferred (production: 8765..8775). Relative upper bound so offline
     * tests can pin their own base port without touching constants. */
    port_last = preferred_port + (HTTPD_PORT_LAST - HTTPD_PORT_FIRST);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        hub_log(TAG, "WSAStartup failed (%d) - web panel unavailable", WSAGetLastError());
        return 0;
    }
    s_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listen == INVALID_SOCKET) {
        hub_log(TAG, "socket() failed (%d)", WSAGetLastError());
        WSACleanup();
        return 0;
    }
    /* NOTE: deliberately NO SO_REUSEADDR - on Windows it would allow binding
     * a port another socket already owns (hijack risk). Default is safe. */
    for (port = preferred_port; port <= port_last; port++) {
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)port);
        if (inet_pton(AF_INET, HTTPD_BIND_ADDR, &sa.sin_addr) != 1) {
            hub_log(TAG, "bind address constant failed to parse (bug)");
            closesocket(s_listen); s_listen = INVALID_SOCKET;
            WSACleanup();
            return 0;
        }
        if (bind(s_listen, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
            bound = 1;
            break;
        }
        if (WSAGetLastError() != WSAEADDRINUSE) {
            hub_log(TAG, "bind %s:%d failed (%d)", HTTPD_BIND_ADDR, port, WSAGetLastError());
            break;
        }
        hub_log(TAG, "port %d busy - scanning next (one pass at startup only)", port);
    }
    if (!bound) {
        hub_log(TAG, "no free port in [%d..%d] - web panel unavailable this session",
                preferred_port, port_last);
        closesocket(s_listen); s_listen = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }
    if (listen(s_listen, 8) != 0) {
        hub_log(TAG, "listen failed (%d)", WSAGetLastError());
        closesocket(s_listen); s_listen = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }
    s_port = port;
    s_running = 1;
    s_thread = CreateThread(NULL, 0, httpd_worker, NULL, 0, NULL);
    if (!s_thread) {
        hub_log(TAG, "CreateThread failed (%lu)", GetLastError());
        s_running = 0;
        closesocket(s_listen); s_listen = INVALID_SOCKET;
        WSACleanup();
        return 0;
    }
    hub_log(TAG, "listening on %s:%d (bind literal, loopback only)", HTTPD_BIND_ADDR, s_port);
    if (actual_port_out) *actual_port_out = s_port;
    return 1;
}

void httpd_stop(void)
{
    if (!s_thread) return;
    hub_log(TAG, "stopping (listener close breaks accept)...");
    s_running = 0;
    if (s_listen != INVALID_SOCKET) {
        closesocket(s_listen);   /* accepted break: accept() returns error */
        s_listen = INVALID_SOCKET;
    }
    /* wait long enough for an in-flight connection to hit its IO deadline */
    WaitForSingleObject(s_thread, 15000);
    CloseHandle(s_thread);
    s_thread = NULL;
    s_port = 0;
    WSACleanup();
    hub_log(TAG, "stopped, port released");
}

int httpd_is_running(void) { return s_thread != NULL; }
int httpd_port(void)       { return s_port; }

void httpd_set_api_handler(httpd_route_fn fn) { s_api = fn; }
