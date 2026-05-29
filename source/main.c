// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.nsp (with APP_JSON)
// Install: sd:/atmosphere/contents/010000000000BD23/exefs.nsp + flags/boot2.flag
//
// v1.7b: Fix boot hang — don't call nifmGetCurrentIpAddress() before nifmInitialize().
//       Restructure main loop: try net_init() directly at boot,
//       use net_is_online() only AFTER nifm is initialized.

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "http_server.h"

/* ---- Global state (declared before all functions) ---- */
static bool g_net_up        = false;
static bool g_bsd_initialized = false;

/* =============================================================== */
/* Sysmodule CRT0 overrides - CRITICAL for boot survival   */
/* =============================================================== */

#define INNER_HEAP_SIZE 0x80000

u32  __nx_applet_type = AppletType_None;
u32  __nx_fs_num_sessions = 2;

void  __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

Result __appInit(void) {
    Result rc;
    rc = smInitialize();
    if (R_FAILED(rc)) return rc;

    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc)) return rc;

    rc = timeInitialize();
    if (R_FAILED(rc)) { }

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) return rc;

    return 0;
}

void __appExit(void) {
    if (g_net_up) {
        http_server_stop();
        nifmExit();
        socketExit();
    } else {
        /* nifm was never initialized, but bsd:ux might be up */
        if (g_bsd_initialized)
            socketExit();
    }
    fsdevUnmountAll();
    fsExit();
    timeExit();
    smExit();
}

/* ---- Constants ---- */
#define LOG_FILE    "sdmc:/switch/pctltcp-sysmodule/sysmodule.log"
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
        u64 now_posix = 0;
        Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
        if (R_FAILED(rc) || now_posix <= 946684800ULL)
            rc = timeGetCurrentTime(TimeType_LocalSystemClock, &now_posix);
        if (R_FAILED(rc) || now_posix <= 946684800ULL)
            rc = timeGetCurrentTime(TimeType_UserSystemClock, &now_posix);

        if (R_SUCCEEDED(rc) && now_posix > 946684800ULL) {
            TimeCalendarTime cal;
            TimeCalendarAdditionalInfo additional;
            rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            if (R_FAILED(rc))
                rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            if (R_SUCCEEDED(rc)) {
                fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                        cal.year, cal.month, cal.day,
                        cal.hour, cal.minute, cal.second, msg);
                fclose(f);
                return;
            }
        }
        fprintf(f, "[?] %s\n", msg);
        fclose(f);
    }
}

static void log_result(const char *ctx, Result rc) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s (0x%08X)",
             ctx, R_SUCCEEDED(rc) ? "OK" : "FAILED", (unsigned)rc);
    log_msg(buf);
}

static void ip_to_str(u32 ip, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%d.%d.%d.%d",
             (int)((ip >>  0) & 0xFF),
             (int)((ip >>  8) & 0xFF),
             (int)((ip >> 16) & 0xFF),
             (int)((ip >> 24) & 0xFF));
}

/* =============================================================== */
/* Network service management                           */
/* =============================================================== */

/* ---- ONE-TIME bsd:ux init ---- */
static Result bsd_init_once(void) {
    if (g_bsd_initialized) return 0;

    SocketInitConfig cfg = {
        .tcp_tx_buf_size  = 0x4000,
        .tcp_rx_buf_size  = 0x4000,
        .tcp_tx_buf_max_size = 0x10000U,
        .tcp_rx_buf_max_size = 0x10000U,
        .udp_tx_buf_size  = 0x1000,
        .udp_rx_buf_size  = 0x4000,
        .sb_efficiency     = 2,
        .bsd_service_type  = BsdServiceType_System,
    };
    Result rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        log_result("socketInitialize", rc);
        return rc;
    }
    g_bsd_initialized = true;
    log_msg("bsd:ux initialized (one-time, process lifetime).");
    return 0;
}

/* ---- Check if network is actually usable ---- */
/* MUST only be called AFTER nifmInitialize() has succeeded (g_net_up == true). */
static bool net_is_online(void) {
    if (!g_net_up) return false;   /* nifm not initialized yet */
    u32 ip = 0;
    Result rc = nifmGetCurrentIpAddress(&ip);
    return (R_SUCCEEDED(rc) && ip != 0);
}

/* ---- Network init (NO socketInitialize!) ---- */
static Result net_init(void) {
    Result rc;

    rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        log_result("nifmInitialize", rc);
        return rc;
    }

    /* Wait for IP to become available (WiFi may still be connecting).
     * nifmInitialize() succeeds immediately, but IP is assigned asynchronously.
     * Without this wait, net_is_online() will immediately return false
     * and the main loop will tear down the network we just set up.
     * Timeout: 30 seconds max. */
    log_msg("Waiting for IP address...");
    u32 ipaddr = 0;
    int waited = 0;
    while (waited < 30) {
        rc = nifmGetCurrentIpAddress(&ipaddr);
        if (R_SUCCEEDED(rc) && ipaddr != 0)
            break;
        svcSleepThread(1000000000ULL); /* 1 second */
        waited++;
    }
    if (ipaddr == 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "WARNING: IP still 0 after %d s (rc=0x%08X)", waited, (unsigned)rc);
        log_msg(buf);
        /* Continue anyway — HTTP server binds INADDR_ANY, will work once IP arrives */
    }

    http_server_start();
    if (!http_server_is_running()) {
        log_msg("HTTP server start FAILED.");
        nifmExit();
        return -1;
    }

    g_net_up = true;
    log_msg("Network services initialized, HTTP server started.");

    char ip[64] = {0};
    if (ipaddr != 0) {
        ip_to_str(ipaddr, ip, sizeof(ip));
        char msg[256];
        snprintf(msg, sizeof(msg), "Web UI: http://%s:%d", ip, HTTP_PORT);
        log_msg(msg);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Web UI: http://<IP not yet assigned>:%d", HTTP_PORT);
        log_msg(msg);
    }
    return 0;
}

/* ---- Network cleanup (NO socketExit!) ---- */
static void net_cleanup(void) {
    if (http_server_is_running()) {
        http_server_stop();
        log_msg("HTTP server stopped (network lost).");
    }
    if (g_net_up) {
        nifmExit();
        log_msg("nifm exited (network down, bsd:ux kept alive).");
    }
    g_net_up = false;
}

/* ---- Network reinit (NO socketExit/socketInitialize!) ---- */
static Result net_reinit(void) {
    net_cleanup();

    Result rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc)) {
        log_result("nifmInitialize (reinit)", rc);
        return rc;
    }

    http_server_start();
    if (!http_server_is_running()) {
        log_msg("HTTP server restart FAILED.");
        nifmExit();
        return -1;
    }

    g_net_up = true;
    log_msg("Network reinit succeeded (bsd:ux untouched).");

    char ip[64] = {0};
    u32 ipaddr = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ipaddr)) && ipaddr != 0) {
        ip_to_str(ipaddr, ip, sizeof(ip));
        char msg[256];
        snprintf(msg, sizeof(msg), "Web UI: http://%s:%d", ip, HTTP_PORT);
        log_msg(msg);
    }
    return 0;
}

/* =============================================================== */
/* Main service init                                      */
/* =============================================================== */
static bool s_base_ready = false;

static Result init_services(void) {
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting (v1.7b)...");

    Result tz_rc = pctl_load_timezone();
    if (R_FAILED(tz_rc)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "WARNING: timezone load failed (0x%08X)", (unsigned)tz_rc);
        log_msg(buf);
    } else {
        log_msg("Timezone rule loaded successfully.");
    }

    {
        u64 test_time = 0;
        Result time_rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &test_time);
        if (R_FAILED(time_rc))
            time_rc = timeGetCurrentTime(TimeType_UserSystemClock, &test_time);
        char buf[128];
        snprintf(buf, sizeof(buf), "Time service: %s (time=%llu, rc=0x%08X)",
                 R_SUCCEEDED(time_rc) ? "OK" : "FAILED",
                 (unsigned long long)test_time, (unsigned)time_rc);
        log_msg(buf);
    }

    s_base_ready = true;
    return 0;
}

/* =============================================================== */
/* sysmodule entry point                                  */
/* =============================================================== */
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    /* Wait a bit for system services to be ready */
    svcSleepThread(15000000000ULL);

    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: Service initialization failed.");
    }

    /* ONE-TIME bsd:ux init */
    rc = bsd_init_once();
    if (R_FAILED(rc)) {
        log_msg("FATAL: bsd_init_once() failed. Entering idle loop.");
        while (1) svcSleepThread(1000000000ULL);
    }

    /* Initial network init with retry.
     * Do NOT call net_is_online() here — nifm is not initialized yet!
     * Just try net_init() directly and retry if network isn't ready. */
    if (s_base_ready) {
        log_msg("Waiting for network to become available...");
        for (int attempt = 0; attempt < 60; attempt++) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) break;
            /* Wait 5 seconds before retry */
            svcSleepThread(5000000000ULL);
        }
        if (R_FAILED(rc)) {
            log_msg("WARNING: Network not available at boot. Will wait for network in main loop.");
        }
    }

    log_msg("pctltcp-sysmodule initialization complete.");

    /* ---- Main loop: proactive network detection ---- */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;

    while (1) {
        svcSleepThread(1000000000ULL);  /* 1 second per loop */
        loop++;

        /* Only check network every 5 seconds */
        if (loop % 5 != 0) continue;

        /* net_is_online() is safe to call now (g_net_up is accurate) */
        bool online = net_is_online();

        if (!online) {
            /* ---- Network is DOWN ---- */
            if (g_net_up) {
                /* Was up before, now lost -> stop everything */
                log_msg("Network lost (sleep?). Stopping HTTP server.");
                net_cleanup();
            }
            /* If already down, do nothing, just wait */
            continue;
        }

        /* ---- Network is UP ---- */
        if (!g_net_up) {
            /* Transition: offline -> online, or first init after boot */
            log_msg("Network restored (wake?). Starting HTTP server.");
            rc = net_init();
            if (R_FAILED(rc)) {
                log_msg("net_init() failed after network restore, will retry.");
            }
            continue;
        }

        /* Network is up AND services are up -> check HTTP server health */
        if (!http_server_is_running()) {
            log_msg("HTTP server down (was running), reinitializing...");
            rc = net_reinit();
            if (R_FAILED(rc))
                log_msg("net_reinit failed, will retry on next cycle.");
        }

        /* IP change detection every 60 seconds */
        if (g_net_up && (loop - last_ip_check >= 60)) {
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

    return 0;
}
