/**
 * http_server.c - Minimal HTTP server for Switch parental control
 *
 * REST API:
 *   GET  /              -> Embedded HTML UI
 *   GET  /api/status    -> JSON: {daily_limit_min, remaining_min, played_min, today, today_name, version}
 *   POST /api/allow     -> Add minutes to today's limit (additive)
 *                          body: minutes=N
 *                          calc: new_limit = current_limit + N
 *   Version: v1.7
 */
#include "http_server.h"
#include "pctl_handler.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <fcntl.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static int       s_server_fd = -1;
static bool      s_running   = false;
static pthread_t s_thread;

/* ------------------------------------------------------------------ */
/* HTTP helpers                                                        */
/* ------------------------------------------------------------------ */
static void http_send(int fd, const char *status, const char *ctype, const char *body)
{
    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "Connection: close\r\n"
        "Content-Length: %d\r\n"
        "\r\n",
        status, ctype, (int)strlen(body));
    write(fd, header, hlen);
    write(fd, body, strlen(body));
}

static int http_read_request(int fd, char *buf, int bufsize)
{
    int total = 0;
    while (total < bufsize - 1) {
        int n = read(fd, buf + total, bufsize - 1 - total);
        if (n <= 0) break;
        total += n;
        buf[total] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* API handlers                                                        */
/* ------------------------------------------------------------------ */
static u32 clamp_remaining_min(u64 remaining_ns)
{
    if (remaining_ns == 0)
        return 0;
    if (remaining_ns > 86400000000000ULL)
        return 0;
    return (u32)NS_TO_MINUTES(remaining_ns);
}

static void api_status(int fd)
{
    u64 remaining_ns = 0;
    u32 daily_limit  = 0;
    u32 remaining_min = 0;
    u32 played_min    = 0;
    int today = 0;

    Result rc = pctl_init();
    if (R_SUCCEEDED(rc)) {
        pctl_get_remaining_time(&remaining_ns);
        pctl_get_daily_limit_minutes(&daily_limit);
        remaining_min = clamp_remaining_min(remaining_ns);
        played_min    = (daily_limit > remaining_min) ? (daily_limit - remaining_min) : 0;
        today = pctl_get_today_day();
        pctl_exit();
    }

    char json[256];
    static const char *day_names[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    snprintf(json, sizeof(json),
        "{\"daily_limit_min\":%u,\"remaining_min\":%u,\"played_min\":%u,\"today\":%d,\"today_name\":\"%s\",\"version\":\"v1.7\"}",
        daily_limit, remaining_min, played_min, today, day_names[today]);

    http_send(fd, "200 OK", "application/json", json);
}

static void api_allow(int fd, const char *body)
{
    unsigned int allow_min = 0;
    const char *p = strstr(body, "minutes");
    if (p) {
        p = strchr(p + 7, '=');
        if (p) allow_min = (unsigned int)atoi(p + 1);
    }

    Result rc = pctl_init();
    if (R_FAILED(rc)) {
        http_send(fd, "200 OK", "application/json", "{\"success\":0,\"error\":\"pctl_init_failed\"}");
        return;
    }

    int today = pctl_get_today_day();

    if (allow_min == 0) {
        rc = pctl_set_day_limit_minutes(today, 0);
    } else {
        u32 daily_limit = 0;
        pctl_get_daily_limit_minutes(&daily_limit);

        u32 new_limit = daily_limit + allow_min;
        if (new_limit > 1440) new_limit = 1440;

        rc = pctl_set_day_limit_minutes(today, new_limit);
    }

    pctl_exit();

    char json[128];
    snprintf(json, sizeof(json),
        "{\"success\":%d}",
        R_SUCCEEDED(rc) ? 1 : 0);

    http_send(fd, "200 OK", "application/json", json);
}

/* Embedded Web UI                                                     */
/* ------------------------------------------------------------------ */
static const char *WEB_HTML =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Switch Timer v1.7</title>"
"<style>"
"body{font-family:sans-serif;background:#1a1a2e;color:#fff;text-align:center;padding:20px;margin:0}"
".box{background:rgba(255,255,255,0.1);border-radius:12px;padding:20px;margin:15px 0}"
".big{font-size:2.5em;font-weight:bold;margin:10px 0}"
".lbl{color:rgba(255,255,255,0.6);font-size:0.9em}"
".row{display:flex;gap:10px;justify-content:center;margin:15px 0}"
".tile{flex:1;background:rgba(255,255,255,0.08);border-radius:10px;padding:14px}"
"input{width:90px;font-size:1.5em;text-align:center;padding:8px;border:none;border-radius:8px;background:rgba(255,255,255,0.15);color:#fff}"
".btns{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;margin:12px 0}"
"button{font-size:1em;padding:10px 18px;border:none;border-radius:8px;background:#3b82f6;color:#fff;cursor:pointer}"
"button:active{transform:scale(0.95)}"
".btn-sm{background:#374151;font-size:0.9em;padding:8px 14px}"
"#msg{margin-top:8px;color:#fbbf24;font-size:0.9em;min-height:20px}"
"</style>"
"</head>"
"<body>"
"<h2>Switch Parental Control <small>v1.7</small></h2>"
"<div class='box'>"
"<div class='row'>"
"<div class='tile'><div class='lbl'>Played</div><div class='big' id='played'>--</div></div>"
"<div class='tile'><div class='lbl'>Remaining</div><div class='big' id='remain'>--</div></div>"
"</div>"
"<div class='lbl' style='margin-top:4px'>Limit: <span id='limit'>--</span> min</div>"
"</div>"
"<div class='box'>"
"<div class='lbl'>Allow to play (minutes)</div>"
"<input type='number' id='min' value='30' min='0' max='300'>"
"<br>"
"<div class='btns'>"
"<button class='btn-sm' onclick='quickSet(15)'>+15</button>"
"<button class='btn-sm' onclick='quickSet(30)'>+30</button>"
"<button class='btn-sm' onclick='quickSet(60)'>+60</button>"
"<button class='btn-sm' onclick='quickSet(90)'>+90</button>"
"</div>"
"<button onclick='allow()'>Confirm</button>"
"<div id='msg'></div>"
"</div>"
"<script>"
"function load(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('limit').textContent=d.daily_limit_min;"
"document.getElementById('remain').textContent=d.remaining_min+'m';"
"document.getElementById('played').textContent=d.played_min+'m';"
"}).catch(()=>{document.getElementById('msg').textContent='Load failed'});"
"}"
"function quickSet(m){"
"document.getElementById('min').value=m;"
"}"
"function allow(){"
"var m=parseInt(document.getElementById('min').value)||0;"
"document.getElementById('msg').textContent='Saving...';"
"fetch('/api/allow',{method:'POST',body:'minutes='+m}).then(r=>r.json()).then(d=>{"
"document.getElementById('msg').textContent=d.success?'Done!':'Failed';"
"setTimeout(function(){document.getElementById('msg').textContent='';load();},1200);"
"}).catch(()=>{document.getElementById('msg').textContent='Error'});"
"}"
"load();"
"setInterval(load,30000);"
"</script>"
"</body>"
"</html>";

/* ------------------------------------------------------------------ */
/* Route dispatcher                                                    */
/* ------------------------------------------------------------------ */
static void handle_request(int fd)
{
    char buf[2048];
    int n = http_read_request(fd, buf, sizeof(buf));
    if (n <= 0) { close(fd); return; }

    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);

    if (strcmp(method, "OPTIONS") == 0) {
        http_send(fd, "204 No Content", "text/plain", "");
        close(fd);
        return;
    }

    char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;

    if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0) {
        http_send(fd, "200 OK", "text/html; charset=utf-8", WEB_HTML);
    } else if (strcmp(path, "/api/status") == 0) {
        api_status(fd);
    } else if (strcmp(path, "/api/allow") == 0 && strcmp(method, "POST") == 0) {
        api_allow(fd, body ? body : "");
    } else {
        http_send(fd, "404 Not Found", "application/json", "{\"error\":\"not found\"}");
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/* Server thread — select() with 1-sec timeout, no self-pipe needed  */
/* ------------------------------------------------------------------ */
static void *http_thread_func(void *arg)
{
    (void)arg;

    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_server_fd, &rfds);

        struct timeval tv;
        tv.tv_sec  = 1;   /* 1-second timeout -> thread wakes up every 1s */
        tv.tv_usec = 0;

        int ret = select(s_server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            /* select() error -> exit thread */
            break;
        }
        if (ret == 0) continue;  /* timeout, re-check s_running */

        if (FD_ISSET(s_server_fd, &rfds)) {
            int client_fd = accept(s_server_fd, NULL, NULL);
            if (client_fd < 0) {
                /* accept error -> socket might be closed, exit thread */
                break;
            }
            handle_request(client_fd);
        }
    }

    s_running = false;  /* ensure flag is false when thread exits */
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

Result http_server_start(void)
{
    struct sockaddr_in addr;
    int optval = 1;

    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_fd < 0) {
        return MAKERESULT(Module_Custom, 1);
    }

    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(s_server_fd);
        s_server_fd = -1;
        return MAKERESULT(Module_Custom, 2);
    }

    if (listen(s_server_fd, 4) < 0) {
        close(s_server_fd);
        s_server_fd = -1;
        return MAKERESULT(Module_Custom, 3);
    }

    s_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 0x10000);  /* 64KB */
    pthread_create(&s_thread, &attr, http_thread_func, NULL);
    pthread_attr_destroy(&attr);

    return 0;  /* Success */
}

void http_server_stop(void)
{
    if (!s_running && s_server_fd < 0) return;

    /* Signal the thread to exit */
    s_running = false;

    /* Close the server socket to unblock accept() / select().
     * On Switch, closing from another thread may not immediately
     * wake select(), but the 1-sec timeout in http_thread_func()
     * guarantees the thread will notice s_running=false within 1 sec. */
    if (s_server_fd >= 0) {
        shutdown(s_server_fd, SHUT_RDWR);
        close(s_server_fd);
        s_server_fd = -1;
    }

    /* Wait for the thread to exit (at most ~1 second due to select timeout) */
    pthread_join(s_thread, NULL);
}

bool http_server_is_running(void)
{
    return s_running;
}
