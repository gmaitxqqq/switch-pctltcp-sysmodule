// pctltcp-sysmodule - Switch Parental Control Web Server (Sysmodule)
// =================================================================
// Background sysmodule: auto-starts at boot, runs forever.
// HTTP server on port 8080 with embedded mobile Web UI.
//
// Install: sd:/atmosphere/contents/0100000000000023/exefs.nsp
// =================================================================

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "pctl_handler.h"
#include "http_server.h"

// ---- Log file (no console in sysmodule) ----
#define LOG_FILE "/switch/pctltcp-sysmodule/sysmodule.log"
#define MAX_LOG_SIZE (100 * 1024)

// ---- Sysmodule boilerplate ----
u32 __nx_applet_type = AppletType_None;

static u8 _heap[0x00480000];
void __libnx_initheap(void) {
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = _heap;
    fake_heap_end   = _heap + sizeof(_heap);
}

// ---- Logging ----
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

// ---- IP to string (libnx has no inet_ntoa) ----
static void ip_to_str(u32 ip, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%d.%d.%d.%d",
             (int)((ip >>  0) & 0xFF),
             (int)((ip >>  8) & 0xFF),
             (int)((ip >> 16) & 0xFF),
             (int)((ip >> 24) & 0xFF));
}

// ---- Service init ----
static Result init_services(void) {
    // Init pctl (best-effort, HTTP UI still works without it)
    Result rc = pctl_init();
    log_result("pctl_init", rc);

    // Init sockets with retry (network may not be up yet)
    rc = MAKERESULT(Module_Libnx, 1);
    for (int i = 0; i < 60 && R_FAILED(rc); i++) {
        rc = socketInitializeDefault();
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(2000000000ULL); // 2s
    }
    log_result("socketInit", rc);
    if (R_FAILED(rc)) {
        if (pctlIsInitialized())
            pctlExit();
        return rc;
    }

    // Init nifm (needed for IP address)
    rc = nifmInitialize(NifmServiceType_User);
    log_result("nifmInit", rc);
    // Non-fatal if this fails

    return 0;
}

static void exit_services(void) {
    log_msg("Shutting down...");
    http_server_stop();
    nifmExit();
    socketExit();
    if (pctlIsInitialized())
        pctlExit();
    log_msg("Stopped.");
}

// ---- Sysmodule entry points ----
void __appInit(void) {
    smInitialize();
    setsysInitialize();
}

void __appExit(void) {
    setsysExit();
    smExit();
}

int main(void) {
    // Wait for system to be ready
    svcSleepThread(15000000000ULL); // 15s

    // Get firmware version
    if (hosversionGet() == 0) {
        Result rc = setsysInitialize();
        if (R_SUCCEEDED(rc)) {
            SetSysFirmwareVersion fw;
            if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
                hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
            setsysExit();
        }
    }

    // Init services
    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: init_services failed!");
        return 1;
    }

    // Start HTTP server (port 8080)
    http_server_start();
    log_msg(http_server_is_running() ? "HTTP server started." : "HTTP server start FAILED.");

    // Log IP
    char ip[64] = {0};
    u32 ipaddr = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ipaddr)) && ipaddr != 0) {
        ip_to_str(ipaddr, ip, sizeof(ip));
    }
    char msg[256];
    snprintf(msg, sizeof(msg), "HTTP server running on http://%s:%d", ip, HTTP_PORT);
    log_msg(msg);

    // Main loop: run forever, health check every 30s
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;

    while (1) {
        svcSleepThread(1000000000ULL); // 1s
        loop++;

        // Health check every 30s
        if ((loop % 30 == 0) && !http_server_is_running()) {
            log_msg("WARNING: HTTP server down, restarting...");
            http_server_start();
            log_msg("HTTP server restarted.");
        }

        // Re-check IP every 5 min
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
                char u[256];
                snprintf(u, sizeof(u), "Web UI: http://%s:%d", new_ip, HTTP_PORT);
                log_msg(u);
            }
            last_ip_check = loop;
        }
    }

    // Unreachable, but kept for completeness
    exit_services();
    return 0;
}
