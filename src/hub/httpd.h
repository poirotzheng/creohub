/* creo-hub M2: minimal HTTP/1.1 server (docs\09 2.1 + 2.7 layers L1/L2/L5/L6/L7).
 * Single accept-loop worker thread, serial per-connection processing,
 * Connection: close (no keep-alive/chunked), loopback-only by compile-time
 * constant. Win7 baseline: BSD-classic winsock only (inet_pton is Vista+),
 * _WIN32_WINNT=0x0601 locks the API face at compile time. */
#ifndef HTTPD_H
#define HTTPD_H

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* L1: bind address is a COMPILE-TIME CONSTANT - no config path can ever
 * expose the hub externally (docs\09 decision: external access = reverse
 * proxy at M4, hub stays loopback). */
#define HTTPD_BIND_ADDR   "127.0.0.1"
#define HTTPD_PORT_FIRST  8765
#define HTTPD_PORT_LAST   8775
#define HTTPD_HDR_MAX     8192          /* L6: request line + headers cap */
#ifndef HTTPD_BODY_MAX                   /* dev builds may override (E11-4 64KB payloads);
                                            production keeps the frozen 64KB L6 cap */
#define HTTPD_BODY_MAX    (64 * 1024)   /* L6: body cap */
#endif
#define HTTPD_IO_TIMEOUT_MS 5000        /* L6: SO_RCVTIMEO/SO_SNDTIMEO + total deadline */

typedef struct HttpdConn {
    SOCKET sock;
} HttpdConn;

typedef struct HttpdRequest {
    char method[8];          /* GET/POST/... (NUL-terminated, <=7 chars) */
    char raw_target[1024];   /* original request target (for logging) */
    char path[512];          /* target minus query; NOT URL-decoded (L7) */
    char query[512];         /* part after first '?' (empty if none) */
    char host[256];          /* Host header value as sent */
    char content_type[128];
    char token[128];         /* X-Hub-Token header value ("" if absent) */
    char origin[256];        /* Origin header value ("" if absent) - b4 L10 */
    int  content_length;     /* -1 when absent; strict single numeric token */
    int  http_minor;         /* 1 for HTTP/1.1, 0 for HTTP/1.0 */
    char* body;              /* malloc'd NUL-terminated (<=BODY_MAX) or NULL */
    int   body_len;
} HttpdRequest;

/* Start the worker thread. Binds HTTPD_BIND_ADDR:preferred_port; on
 * ADDRINUSE scans forward ONCE at startup across the 11-port span from
 * preferred (production: 8765..8775).
 * Returns 1 on success (actual_port_out set), 0 on failure (logged). */
int  httpd_start(int preferred_port, int* actual_port_out);
/* Signal stop, closesocket the listener (breaks accept), join the worker,
 * WSACleanup. Safe to call when not running. */
void httpd_stop(void);
int  httpd_is_running(void);
int  httpd_port(void);

/* A-5 web_api plugs in here. Called ON THE WORKER THREAD; must return 1 if
 * it produced the response (via httpd_respond*), 0 to fall through to 404.
 * Mutating operations must NOT run here - they go through the marshaling
 * queue to the Creo main thread (docs\09 2.2/2.3). */
typedef int (*httpd_route_fn)(const HttpdRequest* req, HttpdConn* conn);
void httpd_set_api_handler(httpd_route_fn fn);

/* Response helpers. NEVER emit Access-Control-Allow-* or Set-Cookie (L5 by
 * construction) and always send Connection: close. body_len < 0 => strlen. */
void httpd_respond(HttpdConn* conn, int status, const char* content_type,
                   const char* body, int body_len);
void httpd_respond_json(HttpdConn* conn, int status, const char* json_text);

#endif /* HTTPD_H */
