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
 * Sysmodule CRT0 overrides — CRITICAL for boot survival
 *
 * Based on working sys-con sysmodule (github.com/o0Zz/sys-con)
 * which uses plain libnx (not Stratosphere) and works as boot2.
 *
 * Three things are REQUIRED for a libnx sysmodule:
 * 1. __nx_applet_type = AppletType_None
 * 2. __libnx_initheap with custom inner heap
 * 3. __appInit that initializes ALL services + sets hosversion
 * ================================================================ */

#define INNER_HEAP_SIZE 0x80000  /* 512 KiB — same as sys-con */

/* ---- CRT0 global overrides ---- */
u32 __nx_applet_type = AppletType_None;  /* 0 — no applet */
u32 __nx_fs_num_sessions = 1;            /* reduce FS sessions for sysmodule */

/* ---- Custom heap ---- */
void __libnx_initheap(void) {
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

/* ---- __appInit — follow sys-con pattern exactly ----
 *
 * CRITICAL: Initialize ALL services here, not in main().
 * The CRT0 expects __appInit to set up everything.
 * Key steps:
 * 1. smInitialize() first (needed for all other services)
 * 2. setsysInitialize() → get firmware version → hosversionSet()
 *    (libnx functions depend on hosversion being set correctly)
 * 3. Initialize all needed services
 * 4. smExit() BEFORE fsdevMountSdmc() (release SM session)
 * 5. fsdevMountSdmc() last
 */
Result __appInit(void) {
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc)) return rc;

    /* Get firmware version — REQUIRED for libnx version-aware functions */
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

    /* Release SM session — fsdevMountSdmc re-connects internally */
    smExit();

    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) return rc;

    return 0;
}

/* ---- __appExit ---- */
void __appExit(void) {
    fsdevUnmountAll();
    fsExit();
    pscmExit();
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

/* ================================================================
 * PSC Power State Monitor
 *
 * When the Switch goes to sleep:
 *   - Stop the HTTP server
 *   - Close network sockets
 *   - Acknowledge sleep to PSC so the system can enter sleep
 *
 * When the Switch wakes up:
 *   - Re-initialize sockets
 *   - Re-start the HTTP server
 *   - Log the new IP address
 * ================================================================ */

static PscPmModule g_psc_module;
static Thread      g_psc_thread;
static UEvent      g_psc_exit_event;
static bool        g_psc_active = false;

/* Are our network services (socket + HTTP) currently up? */
static bool        g_net_up = false;

/* ---- Network service init/cleanup ---- */
static Result net_init(void) {
    Result rc;

    /* Network interface manager (try System first for sysmodule context) */
    rc = nifmInitialize(NifmServiceType_System);
    if (R_FAILED(rc)) {
        rc = nifmInitialize(NifmServiceType_User);
    }
    if (R_FAILED(rc)) {
        log_result("nifmInitialize", rc);
        return rc;
    }

    /* Sockets */
    rc = socketInitializeDefault();
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
                log_msg("PSC: Wake up detected, re-initializing network...");
                net_cleanup();
                /* Small delay to let network interface come up */
                svcSleepThread(2000000000ULL);  /* 2 seconds */
                net_init();
                break;

            case PscPmState_ReadySleep:        /* 2 - preparing to sleep */
                log_msg("PSC: Sleep requested, cleaning up network...");
                net_cleanup();
                break;

            case PscPmState_ReadyShutdown:     /* 5 - preparing to shutdown */
                log_msg("PSC: Shutdown requested, cleaning up...");
                net_cleanup();
                break;

            default:
                break;
        }

        /* MUST acknowledge — system waits for all modules before state change */
        pscPmModuleAcknowledge(&g_psc_module, state);
    }
}

/* ---- PSC init/exit ---- */
static Result psc_monitor_init(void) {
    Result rc;

    /* pscmInitialize is already called in __appInit, but we need to
     * re-initialize since __appExit's pscmExit() will clean it up.
     * Actually, __appExit only runs on process exit, so it's fine
     * to use the session from __appInit. But pscmInitialize is
     * idempotent, so calling it again is safe. */

    /* GetPmModule combines registration + initialization.
     * Module ID: 0x7E (custom homebrew ID)
     * Dependencies: WlanSockets (25) + Nifm (38) — we need network
     *               to come up before us on wake.
     * autoclear: true (event auto-resets after wait) */
    u32 deps[] = { PscPmModuleId_WlanSockets, PscPmModuleId_Nifm };
    rc = pscmGetPmModule(&g_psc_module, 0x7E,
                          deps, sizeof(deps) / sizeof(u32), true);
    if (R_FAILED(rc)) {
        log_result("pscmGetPmModule", rc);
        return rc;
    }

    ueventCreate(&g_psc_exit_event, false);

    rc = threadCreate(&g_psc_thread, psc_thread_func, NULL,
                      NULL, 0x1000, 0x2C, -2);
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
 * Main service init — called once at startup
 *
 * Note: smInitialize/setsys/fsInitialize/pscmInitialize/fsdevMountSdmc
 * are already done in __appInit. Here we initialize application-specific
 * services (pctl, PSC module, network).
 * ================================================================ */
static bool s_base_ready = false;

static Result init_services(void) {
    Result rc;

    /* Create log directory */
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/pctltcp-sysmodule", 0777);

    log_msg("pctltcp-sysmodule starting...");

    /* Parental control service */
    rc = pctl_init();
    log_result("pctl_init", rc);

    /* PSC power state monitor (non-fatal but very useful) */
    rc = psc_monitor_init();
    log_result("psc_monitor_init", rc);

    s_base_ready = true;
    return 0;
}

static void exit_services(void) {
    net_cleanup();
    psc_monitor_exit();
    if (pctl_is_initialized()) pctl_exit();
    log_msg("pctltcp-sysmodule exiting.");
}

/* ================================================================
 * sysmodule entry point
 * Built with switch.specs (application CRT0) -> int main()
 * Installed as boot2 sysmodule via exefs.nsp + boot2.flag
 * NPDM grants all service access and needed syscalls.
 * ================================================================ */
int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Wait for system to be ready before initializing services.
     * At boot2, some services might not be fully up yet. */
    svcSleepThread(5000000000ULL);  /* 5 seconds */

    /* Initialize services (pctl, PSC monitor) */
    Result rc = init_services();
    if (R_FAILED(rc)) {
        log_msg("FATAL: Service initialization failed");
    }

    /* Initialize network (socket, nifm, HTTP server) with retry */
    if (s_base_ready) {
        for (int attempt = 0; attempt < 120; attempt++) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) break;
            /* Network may not be up yet at boot */
            svcSleepThread(2000000000ULL);  /* 2 seconds */
        }
        if (R_FAILED(rc)) {
            log_msg("WARNING: Network init failed after 120 retries");
        }
    }

    log_msg("pctltcp-sysmodule initialization complete.");

    /* ---- Main loop ----
     * Keep the sysmodule alive forever.
     * PSC thread handles sleep/wake automatically.
     * Main loop does lightweight housekeeping:
     * 1. Restart HTTP server if it crashed (not due to sleep)
     * 2. Retry network init if it failed at startup
     * 3. Check for IP changes
     */
    u64 loop = 0;
    char last_ip[64] = {0};
    u64 last_ip_check = 0;

    while (1) {
        svcSleepThread(1000000000ULL);  /* 1 second */
        loop++;

        /* If network isn't up yet (startup retry), try again */
        if (!g_net_up && s_base_ready && (loop % 10 == 0)) {
            rc = net_init();
            if (R_SUCCEEDED(rc)) {
                log_msg("Network init succeeded on retry!");
            }
        }

        /* Restart HTTP server if it died (not due to sleep —
         * PSC handles sleep/wake. This catches unexpected crashes.) */
        if (g_net_up && (loop % 30 == 0) && !http_server_is_running()) {
            log_msg("WARNING: HTTP server down (unexpected), restarting...");
            http_server_start();
            if (http_server_is_running()) {
                log_msg("HTTP server restarted.");
            } else {
                log_msg("HTTP server restart failed, will retry full net_init...");
                net_cleanup();
                /* net_init will be retried on next loop iteration */
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
