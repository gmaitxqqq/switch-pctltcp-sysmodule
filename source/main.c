// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.elf
// CI: mknpdm.py + mknsp.py -> exefs.nsp
// Install: sd:/atmosphere/contents/0100000000000023/exefs.nsp + flags/boot2.flag

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "http_server.h"

#define PROGRAM_ID  0x0100000000000023ULL
#define LOG_FILE    "/switch/pctltcp-sysmodule/sysmodule.log"
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

/* ---- Service init ----
 * Based on MissionControl's initialization sequence.
 * Key differences from the old version:
 * 1. Initialize sm FIRST (required for all other services)
 * 2. Initialize FS and mount SD card (for logging)
 * 3. Don't exit on single service failure - keep running
 * 4. Use NifmServiceType_System for sysmodule context
 */
static bool s_services_ready = false;

static Result init_services(void) {
    Result rc;
    
    /* Step 1: Service Manager (required for everything else) */
    rc = smInitialize();
    if (R_FAILED(rc)) {
        /* Can't even log without SM... just sleep and retry */
        return rc;
    }
    
    /* Step 2: FS initialization + mount SD card (for logging) */
    rc = fsInitialize();
    if (R_SUCCEEDED(rc)) {
        rc = fsMountSdcard("sdmc");
        if (R_SUCCEEDED(rc)) {
            /* Create log directory */
            mkdir("sdmc:/switch", 0777);
            mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);
        }
    }
    
    log_msg("pctltcp-sysmodule starting...");
    
    /* Step 3: System settings (non-fatal if fails) */
    rc = setsysInitialize();
    log_result("setsysInitialize", rc);
    
    /* Step 4: Parental control service */
    rc = pctl_init();
    log_result("pctl_init", rc);
    
    /* Step 5: Network interface (try System first, fall back to User) */
    rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) {
        rc = nifmInitialize(NifmServiceType_User);
    }
    log_result("nifmInitialize", rc);
    
    /* Step 6: Sockets with retry (network may not be up yet at boot) */
    rc = -1;
    for (int i = 0; i < 120; i++) {  /* 120 retries = ~4 minutes */
        rc = socketInitializeDefault();
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(2000000000ULL);  /* 2 seconds */
    }
    log_result("socketInitialize", rc);
    
    if (R_FAILED(rc)) {
        log_msg("WARNING: Socket init failed after 120 retries, will keep trying in main loop");
    }
    
    s_services_ready = R_SUCCEEDED(rc);
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
    fsUnmountSdcard();
    fsExit();
    smExit();
}

/* ---- sysmodule entry point ----
 * Built with switch.specs (application CRT0) -> int main()
 * Installed as boot2 sysmodule via exefs.nsp + boot2.flag
 * NPDM grants all service access and needed syscalls.
 */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Wait for system to be ready before initializing services.
     * At boot, services may not be available yet.
     * MissionControl uses sm::Initialize() in InitializeSystemModule(),
     * but we're not using stratosphere, so we sleep first. */
    svcSleepThread(5000000000ULL);  /* 5 seconds */

    /* Initialize services (with retries) */
    Result rc = init_services();
    if (R_FAILED(rc)) {
        /* Even if some services fail, keep trying */
        log_msg("WARNING: Some services failed to initialize, will retry in main loop");
    }

    /* Start HTTP server if sockets are ready */
    if (s_services_ready) {
        http_server_start();
        log_msg(http_server_is_running() ? "HTTP server started." : "HTTP server start FAILED.");
    }

    /* Log IP address */
    char ip[64] = {0};
    u32 ipaddr = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ipaddr)) && ipaddr != 0) {
        ip_to_str(ipaddr, ip, sizeof(ip));
        char msg[256];
        snprintf(msg, sizeof(msg), "Web UI: http://%s:%d", ip, HTTP_PORT);
        log_msg(msg);
    }

    /* ---- Main loop ----
     * Keep the sysmodule alive forever.
     * Periodically check:
     * 1. Restart HTTP server if it crashed
     * 2. Retry socket init if it failed at startup
     * 3. Check for IP changes
     */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;

    while (1) {
        svcSleepThread(1000000000ULL);  /* 1 second */
        loop++;

        /* If sockets aren't initialized yet, retry */
        if (!s_services_ready && (loop % 10 == 0)) {
            rc = socketInitializeDefault();
            if (R_SUCCEEDED(rc)) {
                log_msg("Socket init succeeded on retry!");
                s_services_ready = true;
                http_server_start();
                log_msg(http_server_is_running() ? "HTTP server started (delayed)." : "HTTP server start FAILED (delayed).");
            }
        }

        /* Restart HTTP server if it died */
        if (s_services_ready && (loop % 30 == 0) && !http_server_is_running()) {
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
            if (new_ip[0] && strcmp(last_ip, new_ip) != 0) {
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
