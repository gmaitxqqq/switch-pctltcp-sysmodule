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
#include <switch.h>

static int       s_server_fd = -1;
static bool      s_running   = false;
static pthread_t s_thread;

/* ── helpers ── */
static void http_send(int fd, const char *status, const char *ctype, const char *body)
{
    char header[512];
    int  hlen = snprintf(header, sizeof(header),
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

/* ── API ── */
static void api_status(int fd)
{
    u64 remaining_ns = 0;
    u32 daily_limit = 0;
    pctl_get_remaining_time(&remaining_ns);
    pctl_get_daily_limit_minutes(&daily_limit);
    char json[256];
    snprintf(json, sizeof(json),
        "{\"daily_limit_min\":%u,\"remaining_min\":%llu}",
        daily_limit, (unsigned long long)NS_TO_MINUTES(remaining_ns));
    http_send(fd, "200 OK", "application/json", json);
}

static void api_set(int fd, const char *body)
{
    unsigned int minutes = 0;
    const char *p = strstr(body, "minutes");
    if (p) { p = strchr(p + 7, '='); if (p) minutes = (unsigned int)atoi(p + 1); }
    Result rc = pctl_set_daily_limit_minutes(minutes);
    char json[128];
    snprintf(json, sizeof(json),
        "{\"success\":%d}", R_SUCCEEDED(rc) ? 1 : 0);
    http_send(fd, "200 OK", "application/json", json);
}

static const char *WEB_HTML =
"<!DOCTYPE html>"
"<html><head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Switch Timer</title>"
"<style>"
"body{font-family:sans-serif;background:#1a1a2e;color:#fff;text-align:center;padding:20px;margin:0}"
".box{background:rgba(255,255,255,0.1);border-radius:12px;padding:20px;margin:15px 0}"
".big{font-size:2.5em;font-weight:bold;margin:10px 0}"
".lbl{color:rgba(255,255,255,0.6);font-size:0.9em}"
"input{width:100px;font-size:1.5em;text-align:center;padding:8px;border:none;border-radius:8px;background:rgba(255,255,255,0.15);color:#fff}"
"button{font-size:1.2em;padding:12px 30px;border:none;border-radius:8px;background:#3b82f6;color:#fff;margin-top:15px;cursor:pointer}"
"button:active{transform:scale(0.95)}"
"#msg{margin-top:10px;color:#fbbf24;font-size:0.9em;height:20px}"
"</style></head><body>"
"<h2>Switch Parental Control</h2>"
"<div class='box'><div class='lbl'>Daily Limit</div>"
"<div class='big' id='limit'>--</div></div>"
"<div class='box'><div class='lbl'>Remaining</div>"
"<div class='big' id='remain'>--</div></div>"
"<div class='box'><div class='lbl'>Set Today's Limit (minutes)</div>"
"<input type='number' id='min' value='60' min='0' max='1440'>"
"<br><button onclick='setLimit()'>Set</button>"
"<div id='msg'></div></div>"
"<script>"
"function load(){"
"fetch('/api/status').then(r=>r.json()).then(d=>{"
"document.getElementById('limit').textContent=d.daily_limit_min+'m';"
"document.getElementById('remain').textContent=d.remaining_min+'m';"
"}).catch(e=>{document.getElementById('msg').textContent='Load failed'});"
"}"
"function setLimit(){"
"var m=document.getElementById('min').value;"
"fetch('/api/set',{method:'POST',body:'minutes='+m}).then(r=>r.json()).then(d=>{"
"document.getElementById('msg').textContent=d.success?'OK':'Failed';"
"setTimeout(load,500);"
"}).catch(e=>{document.getElementById('msg').textContent='Error'});"
"}"
"load();"
"</script></body></html>";

/* ── dispatcher ── */
static void handle_request(int fd)
{
    char buf[2048];
    int  n = http_read_request(fd, buf, sizeof(buf));
    if (n <= 0) { close(fd); return; }
    char method[16] = {0}, path[256] = {0};
    sscanf(buf, "%15s %255s", method, path);
    if (strcmp(method, "OPTIONS") == 0) {
        http_send(fd, "204 No Content", "text/plain", "");
        close(fd); return;
    }
    char *body = strstr(buf, "\r\n\r\n");
    if (body) body += 4;
    if (strcmp(path, "/") == 0 && strcmp(method, "GET") == 0)
        http_send(fd, "200 OK", "text/html; charset=utf-8", WEB_HTML);
    else if (strcmp(path, "/api/status") == 0)
        api_status(fd);
    else if (strcmp(path, "/api/set") == 0)
        api_set(fd, body ? body : "");
    else
        http_send(fd, "404 Not Found", "application/json",
                  "{\"error\":\"not found\"}");
    close(fd);
}

/* ── thread ── */
static void *http_thread_func(void *arg)
{
    (void)arg;
    while (s_running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(s_server_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        int ret = select(s_server_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret <= 0) continue;
        if (FD_ISSET(s_server_fd, &rfds)) {
            int client_fd = accept(s_server_fd, NULL, NULL);
            if (client_fd >= 0) handle_request(client_fd);
        }
    }
    return NULL;
}

/* ── public API ── */
void http_server_start(void)
{
    struct sockaddr_in addr;
    s_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_server_fd < 0) return;
    int optval = 1;
    setsockopt(s_server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(s_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(s_server_fd); s_server_fd = -1; return; }
    if (listen(s_server_fd, 4) < 0) { close(s_server_fd); s_server_fd = -1; return; }
    s_running = true;
    pthread_create(&s_thread, NULL, http_thread_func, NULL);
}

void http_server_stop(void)
{
    s_running = false;
    if (s_server_fd >= 0) {
        shutdown(s_server_fd, SHUT_RDWR);
        close(s_server_fd);
        s_server_fd = -1;
    }
    pthread_join(s_thread, NULL);
}

bool http_server_is_running(void)
{
    return s_running;
}
