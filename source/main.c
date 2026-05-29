// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.nsp
// Install: sd:/atmosphere/contents/<TID>/exefs.nsp + flags/boot2.flag

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "http_server.h"

#define LOG_FILE "/switch/pctltcp-sysmodule/sysmodule.log"
#define MAX_LOG_SIZE (100 * 1024)

/* ---- Logging ---- */
static void rotate_log_if_needed(void) {
    FILE *f = fopen(LOG_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fclose(f);
        if (size > MAX_LOG_SIZE) {
            char old[256];
            snprintf(old, sizeof(old), "%s.old", LOG_FILE);
            rename(LOG_FILE, old);
        }
    }
}

void log_msg(const char *msg) {
    rotate_log_if_needed();
    FILE *f = fopen(LOG_FILE, "a");
    if (f) {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                t->tm_hour, t->tm_min, t->tm_sec, msg);
        fclose(f);
    }
}

static void log_result(const char *ctx, Result rc) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s (0x%08X)",
             ctx, R_SUCCEEDED(rc) ? "OK" : "FAILED", (unsigned)rc);
    log_msg(buf);
}

/* ---- IP to string ---- */
static void ip_to_str(u32 ip, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%d.%d.%d.%d",
             (int)((ip >>  0) & 0xFF),
             (int)((ip >>  8) & 0xFF),
             (int)((ip >> 16) & 0xFF),
             (int)((ip >> 24) & 0xFF));
}

/* ---- Service init ---- */
static Result init_services(void) {
    Result rc = pctl_init();
    log_result("pctl_init", rc);

    /* Init sockets with retry (network may not be up yet) */
    rc = -1;
    for (int i = 0; i < 60; i++) {
        rc = socketInitializeDefault();
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(2000000000ULL);
    }
    log_result("socketInit", rc);
    if (R_FAILED(rc)) {
        if (pctl_is_initialized())
            pctl_exit();
        return rc;
    }

    rc = nifmInitialize(NifmServiceType_User);
    log_result("nifmInit", rc);
    /* Non-fatal if this fails */

    return rc;
}

/* ---- Service exit ---- */
static void exit_services(void) {
    if (http_server_is_running())
        http_server_stop();
    if (pctl_is_initialized())
        pctl_exit();
    nifmExit();
    socketExit();
    setsysExit();
    smExit();
}

/* ---- sysmodule entry point ----
 * Built with switch.specs (application CRT0) -> int main()
 * Installed as boot2 sysmodule via exefs.nsp + boot2.flag
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Init system services */
    Result rc = smInitialize();
    if (R_FAILED(rc)) return 1;

    rc = setsysInitialize();
    if (R_FAILED(rc)) { smExit(); return 1; }

    /* Create log directory */
    mkdir("/switch", 0777);
    mkdir("/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting...");

    /* Wait for system to be ready */
    svcSleepThread(15000000000ULL);

    rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: init_services failed!");
        exit_services();
        return 1;
    }

    http_server_start();
    log_msg(http_server_is_running() ? "HTTP server started." : "HTTP server start FAILED.");

    /* Log IP address */
    char ip[64] = {0};
    u32 ipaddr = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ipaddr)) && ipaddr != 0) {
        ip_to_str(ipaddr, ip, sizeof(ip));
    }
    {
        char msg[256];
        snprintf(msg, sizeof(msg), "HTTP server running on http://%s:%d", ip, HTTP_PORT);
        log_msg(msg);
    }

    /* Main loop - keep running forever */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;

    while (1) {
        svcSleepThread(1000000000ULL);
        loop++;

        /* Restart HTTP server if it died */
        if ((loop % 30 == 0) && !http_server_is_running()) {
            log_msg("WARNING: HTTP server down, restarting...");
            http_server_start();
            log_msg("HTTP server restarted.");
        }

        /* Check for IP change every 5 minutes */
        if (loop - last_ip_check >= 300) {
            char new_ip[64] = {0};
            u32 a = 0;
            if (R_SUCCEEDED(nifmGetCurrentIpAddress(&a)) && a != 0) {
                ip_to_str(a, new_ip, sizeof(new_ip));
            }
            if (strcmp(last_ip, new_ip) != 0) {
                char m[256];
                snprintf(m, sizeof(m), "IP changed: %s -> %s",
                         last_ip[0] ? last_ip : "(none)", new_ip);
                log_msg(m);
                strcpy(last_ip, new_ip);
                {
                    char u[256];
                    snprintf(u, sizeof(u), "Web UI: http://%s:%d", new_ip, HTTP_PORT);
                    log_msg(u);
                }
            }
            last_ip_check = loop;
        }
    }

    /* Unreachable, but keeps compiler happy */
    exit_services();
    return 0;
}
