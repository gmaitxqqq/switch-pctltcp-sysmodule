// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.nsp
// Install: sd:/atmosphere/contents/0100000000000023/exefs.nsp

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

/* ---- App hooks (called by switch.specs CRT0) ---- */
void userAppInit(void);
void userAppExit(void);

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

    return 0;
}

/* ---- App hooks ---- */
void userAppInit(void) {
    smInitialize();
    setsysInitialize();
}

void userAppExit(void) {
    setsysExit();
    smExit();
}

/* ---- sysmodule entry point (switch_sysmodule.specs) ---- */
void userAppMain(void) {
    (void)argc;
    (void)argv;

    mkdir("/switch", 0777);
    mkdir("/switch/pctltcp-sysmodule", 0777);

    svcSleepThread(15000000000ULL);

    /* Get firmware version */
    {
        Result rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
                hosversionSet(HOSVERSION_MAKE(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }

    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: init_services failed!");
        return 1;
    }

    http_server_start();
    log_msg(http_server_is_running() ? "HTTP server started." : "HTTP server start FAILED.");

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

    /* Main loop */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;

    while (1) {
        svcSleepThread(1000000000ULL);
        loop++;

        if ((loop % 30 == 0) && !http_server_is_running()) {
            log_msg("WARNING: HTTP server down, restarting...");
            http_server_start();
            log_msg("HTTP server restarted.");
        }

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

    return 0;
}
