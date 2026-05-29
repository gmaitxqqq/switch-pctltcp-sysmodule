// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.nsp (with APP_JSON)
// Install: sd:/atmosphere/contents/010000000000BD23/exefs.nsp + flags/boot2.flag

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>

#include "pctl_handler.h"
#include "http_server.h"

/* ================================================================
 * Sysmodule CRT0 overrides - CRITICAL for boot survival
 * ================================================================ */

#define INNER_HEAP_SIZE 0x80000  /* 512 KiB - same as sys-con */

/* ---- CRT0 global overrides ---- */
u32 __nx_applet_type = AppletType_None;  /* 0 - no applet */
u32 __nx_fs_num_sessions = 2;            /* FS sessions for sysmodule */

/* ---- Custom heap ---- */
void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

/* ---- __appInit - initialize all needed services ---- */
Result __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) return rc;

    /* Get firmware version - REQUIRED for libnx version-aware functions */
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        rc = setsysGetFirmwareVersion(&fw);
        if (R_SUCCEEDED(rc)) {
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        }
        setsysExit();
    }

    rc = pscmInitialize();
    if (R_FAILED(rc)) return rc;

    rc = fsInitialize();
    if (R_FAILED(rc)) return rc;

    /* Time service - REQUIRED for localtime()/time() to work correctly
     * in sysmodule context. Without this, time(NULL) returns epoch 0
     * (Thursday 1970-01-01), causing pctl_get_today_day() to always
     * return 4 (Thursday) instead of the actual day. */
    rc = timeInitialize();
    if (R_FAILED(rc)) {
        /* Non-fatal: pctl day-of-week may be wrong, but module can still run */
    }

    /* DO NOT call smExit() here! We need SM for nifm/pctl later. */
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) return rc;

    return 0;
}

/* ---- __appExit ---- */
void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    timeExit();
    pscmExit();
    smExit();  /* SM is kept alive for the entire process lifetime */
}

/* ---- Constants ---- */
#define PROGRAM_ID  0x010000000000BD23ULL
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
        /* Try all clock sources for reliable timestamps */
        u64 now_posix = 0;
        Result rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &now_posix);
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_LocalSystemClock, &now_posix);
        }
        if (R_FAILED(rc) || now_posix <= 946684800ULL) {
            rc = timeGetCurrentTime(TimeType_UserSystemClock, &now_posix);
        }

        if (R_SUCCEEDED(rc) && now_posix > 946684800ULL) {
            TimeCalendarTime cal;
            TimeCalendarAdditionalInfo additional;

            /* Try WithMyRule first (auto timezone) */
            rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            if (R_FAILED(rc)) {
                /* Try explicit timezone rule (loaded at startup) */
                rc = timeToCalendarTimeWithMyRule(now_posix, &cal, &additional);
            }

            if (R_SUCCEEDED(rc)) {
                fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] %s\n",
                        cal.year, cal.month, cal.day,
                        cal.hour, cal.minute, cal.second, msg);
                fclose(f);
                return;
            }
        }
        /* Fallback: use raw epoch or simple format */
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

/* ---- IP to string ---- */
static void ip_to_str(u32 ip, char *buf, size_t bufsize) {
    snprintf(buf, bufsize, "%d.%d.%d.%d",
             (int)((ip >>  0) & 0xFF),
             (int)((ip >>  8) & 0xFF),
             (int)((ip >> 16) & 0xFF),
             (int)((ip >> 24) & 0xFF));
}

/* ================================================================
 * PSC Power State Monitor
 * ================================================================ */

static PscPmModule g_psc_module;
static Thread      g_psc_thread;
static UEvent      g_psc_exit_event;
static bool        g_psc_active = false;

/* Are our network services (socket + HTTP) currently up? */
static bool        g_net_up = false;

/* Flag: PSC wake event received, main loop should reinit network.
 * This MUST be set by PSC thread and cleared by main loop. */
static volatile bool g_pending_wake_reinit = false;

/* ---- Network service init/cleanup ---- */
static Result net_init(void) {
    Result rc;

    /* Network interface manager - try System type for sysmodule context */
    rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) {
        rc = nifmInitialize(NifmServiceType_User);
    }
    if (R_FAILED(rc)) {
        log_result("nifmInitialize", rc);
        return rc;
    }

    /* Sockets - use System service type with explicit config */
    SocketInitConfig cfg = {
        .tcp_tx_buf_size = 0x4000,
        .tcp_rx_buf_size = 0x4000,
        .tcp_tx_buf_max_size = 0x10000,
        .tcp_rx_buf_max_size = 0x10000,
        .udp_tx_buf_size = 0x1000,
        .udp_rx_buf_size = 0x4000,
        .sb_efficiency = 2,
        .bsd_service_type = BsdServiceType_System,
    };
    rc = socketInitialize(&cfg);
    if (R_FAILED(rc)) {
        log_result("socketInitialize", rc);
        nifmExit();
        return rc;
    }

    /* HTTP server */
    http_server_start();
    if (!http_server_is_running()) {
        log_msg("HTTP server start FAILED.");
        socketExit();
        nifmExit();
        return -1;
    }

    g_net_up = true;
    log_msg("Network services initialized, HTTP server started.");

    /* Log IP address */
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

static void net_cleanup(void) {
    if (!g_net_up) return;

    if (http_server_is_running()) {
        http_server_stop();
        log_msg("HTTP server stopped.");
    }

    socketExit();
    nifmExit();
    g_net_up = false;
    log_msg("Network services cleaned up.");
}

/* Light cleanup for PSC sleep — stop HTTP server only.
 * DO NOT exit nifm/socket service sessions during sleep!
 * Exiting them causes a deadlock: the PSC system tries to tell
 * WlanSockets/Nifm modules to sleep, but finds their service
 * sessions already closed, causing a system freeze.
 * Instead, just stop our HTTP server and close our socket.
 * The service sessions survive sleep/wake and remain valid. */
static void net_sleep(void) {
    if (!g_net_up) return;

    if (http_server_is_running()) {
        http_server_stop();
        log_msg("HTTP server stopped for sleep.");
    }
    /* DO NOT call socketExit() or nifmExit() here! */
    g_net_up = false;
}

/* Light init for PSC wake — restart HTTP server only.
 * Service sessions (nifm, socket) should still be valid after wake.
 * If simple restart fails, fall back to full reinit. */
static Result net_wake(void) {
    if (g_net_up) return 0;

    /* Try simple restart first — nifm/socket sessions should still be valid */
    http_server_start();
    if (http_server_is_running()) {
        g_net_up = true;
        log_msg("Network restored after wake (light init).");

        /* Log IP address */
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

    /* Simple restart failed — socket might be invalid after wake.
     * Fall back to full service reinit. */
    log_msg("Light wake init failed, trying full reinit...");
    socketExit();
    nifmExit();
    return net_init();
}

/* ---- PSC thread ---- */
static void psc_thread_func(void *arg) {
    (void)arg;

    Waiter psc_waiter  = waiterForEvent(&g_psc_module.event);
    Waiter exit_waiter = waiterForUEvent(&g_psc_exit_event);

    while (true) {
        s32 idx = -1;
        Result rc = waitMulti(&idx, UINT64_MAX, psc_waiter, exit_waiter);
        if (R_FAILED(rc)) continue;

        /* Exit signal */
        if (idx == 1) break;

        /* Get power state change request */
        PscPmState state;
        u32 flags;
        rc = pscPmModuleGetRequest(&g_psc_module, &state, &flags);
        if (R_FAILED(rc)) continue;

        switch (state) {
            case PscPmState_Awake:             /* 0 - system fully awake */
            case PscPmState_ReadyAwaken:       /* 1 - waking up from sleep */
                /* CRITICAL: Acknowledge IMMEDIATELY on wake!
                 * The system blocks all wake-up completion until every
                 * PSC module acknowledges. If we do net_init() here
                 * (which calls nifmInitialize, socketInitialize, etc.),
                 * the system deadlocks because those services haven't
                 * finished waking up yet. Instead, set a flag and let
                 * the main loop handle reinit after a safe delay. */
                pscPmModuleAcknowledge(&g_psc_module, state);
                g_pending_wake_reinit = true;
                break;

            case PscPmState_ReadySleep:        /* 2 - preparing to sleep */
                /* CRITICAL: Use net_sleep() NOT net_cleanup()!
                 * Calling socketExit()/nifmExit() here causes the
                 * system to freeze on sleep because the PSC scheduler
                 * needs WlanSockets/Nifm modules to complete their own
                 * sleep transition, but we've already destroyed their
                 * service sessions. Just stop our HTTP server and
                 * acknowledge — the service sessions survive sleep. */
                net_sleep();
                pscPmModuleAcknowledge(&g_psc_module, state);
                break;

            case PscPmState_ReadyShutdown:     /* 5 - preparing to shutdown */
                net_cleanup();
                pscPmModuleAcknowledge(&g_psc_module, state);
                break;

            default:
                pscPmModuleAcknowledge(&g_psc_module, state);
                break;
        }
    }
}

/* ---- PSC init/exit ---- */
static Result psc_monitor_init(void) {
    Result rc;

    u32 deps[] = { PscPmModuleId_WlanSockets, PscPmModuleId_Nifm };
    rc = pscmGetPmModule(&g_psc_module, 0x7E,
                          deps, sizeof(deps) / sizeof(u32), true);
    if (R_FAILED(rc)) {
        log_result("pscmGetPmModule", rc);
        return rc;
    }

    ueventCreate(&g_psc_exit_event, false);

    /* Stack increased to 16KB - 4KB was too small */
    rc = threadCreate(&g_psc_thread, psc_thread_func, NULL,
                      NULL, 0x4000, 0x2C, -2);
    if (R_FAILED(rc)) {
        log_result("threadCreate(psc)", rc);
        pscPmModuleClose(&g_psc_module);
        return rc;
    }

    rc = threadStart(&g_psc_thread);
    if (R_FAILED(rc)) {
        log_result("threadStart(psc)", rc);
        threadClose(&g_psc_thread);
        pscPmModuleClose(&g_psc_module);
        return rc;
    }

    g_psc_active = true;
    log_msg("PSC power monitor started.");
    return 0;
}

static void psc_monitor_exit(void) {
    if (!g_psc_active) return;

    g_psc_active = false;
    ueventSignal(&g_psc_exit_event);
    pscPmModuleFinalize(&g_psc_module);
    threadWaitForExit(&g_psc_thread);
    threadClose(&g_psc_thread);
    pscPmModuleClose(&g_psc_module);
    log_msg("PSC power monitor stopped.");
}

/* ================================================================
 * Main service init - called once at startup
 * ================================================================ */
static bool s_base_ready = false;

static Result init_services(void) {
    Result rc;

    /* Create log directory */
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting...");

    /* Load timezone rule for correct day-of-week calculation.
     * This MUST be called after timeInitialize() (which is in __appInit).
     * Without this, timeToCalendarTimeWithMyRule() may fail in sysmodule
     * context, causing pctl_get_today_day() to return the wrong day. */
    {
        Result tz_rc = pctl_load_timezone();
        if (R_FAILED(tz_rc)) {
            char buf[128];
            snprintf(buf, sizeof(buf), "WARNING: timezone load failed (0x%08X), day-of-week may be wrong", (unsigned)tz_rc);
            log_msg(buf);
        } else {
            log_msg("Timezone rule loaded successfully.");
        }
    }

    /* Log time service status */
    {
        u64 test_time = 0;
        Result time_rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &test_time);
        if (R_FAILED(time_rc)) {
            time_rc = timeGetCurrentTime(TimeType_UserSystemClock, &test_time);
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "Time service: %s (time=%llu, rc=0x%08X)",
                 R_SUCCEEDED(time_rc) ? "OK" : "FAILED",
                 (unsigned long long)test_time, (unsigned)time_rc);
        log_msg(buf);
    }

    /* PSC power state monitor (non-fatal but very useful) */
    rc = psc_monitor_init();
    log_result("psc_monitor_init", rc);

    s_base_ready = true;
    return 0;
}

static void exit_services(void) {
    net_cleanup();
    psc_monitor_exit();
    log_msg("pctltcp-sysmodule exiting.");
}

/* ================================================================
 * sysmodule entry point
 * ================================================================ */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* At boot2, many services are not yet registered with SM.
     * Wait longer for the system to finish initializing services.
     * Do NOT aggressively retry in a tight loop - this starves
     * the scheduler and can destabilize other boot2 modules. */
    svcSleepThread(15000000000ULL);  /* 15 seconds */

    /* Initialize services (pctl, PSC monitor) */
    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: Service initialization failed, entering idle loop.");
    }

    /* Initialize network (socket, nifm, HTTP server) with sparse retry.
     * Use long intervals to avoid flooding SM with session requests. */
    if (s_base_ready) {
        for (int attempt = 0; attempt < 30; attempt++) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) break;
            svcSleepThread(5000000000ULL);  /* 5 seconds */
        }
        if (R_FAILED(rc)) {
            log_msg("WARNING: Network init failed after 30 retries");
        }
    }

    log_msg("pctltcp-sysmodule initialization complete.");

    /* ---- Main loop ----
     * Keep the sysmodule alive forever.
     * PSC thread handles sleep/wake automatically.
     */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;

    while (1) {
        svcSleepThread(1000000000ULL);  /* 1 second */
        loop++;

        /* Handle deferred wake reinit from PSC thread.
         * Must be done here (not in PSC thread) because the system
         * needs PSC acknowledge before services are fully awake. */
        if (g_pending_wake_reinit) {
            g_pending_wake_reinit = false;
            /* Wait for system services to stabilize after wake.
             * WiFi reconnection takes several seconds. */
            svcSleepThread(5000000000ULL);  /* 5 seconds */
            if (!g_net_up) {
                net_wake();
            }
        }

        /* If network is not up yet (startup retry), try again every 30s */
        if (!g_net_up && s_base_ready && (loop % 30 == 0)) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) {
                log_msg("Network init succeeded on retry!");
            }
        }

        /* Restart HTTP server if it died (not due to sleep) */
        if (g_net_up && (loop % 60 == 0) && !http_server_is_running()) {
            log_msg("WARNING: HTTP server down (unexpected), restarting...");
            http_server_start();
            if (http_server_is_running()) {
                log_msg("HTTP server restarted.");
            } else {
                log_msg("HTTP server restart failed, will retry full net_init...");
                net_cleanup();
            }
        }

        /* Check for IP change every 5 minutes (only if net is up) */
        if (g_net_up && (loop - last_ip_check >= 300)) {
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

    /* Unreachable */
    exit_services();
    return 0;
}
