// pctltcp-sysmodule - Switch Parental Control Web Server (boot2 sysmodule)
// Build: make -> pctltcp-sysmodule.nsp (with APP_JSON)
// Install: sd:/atmosphere/contents/010000000000BD23/exefs.nsp + flags/boot2.flag
//
// v1.7d: Fix false "Network lost" detection.
//         Wait for IP address assignment in net_init() to avoid premature timeout.
//         Proactive network detection: check network status in main loop,
//         restart HTTP server when network is restored after sleep/wake.

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

__attribute__((format(printf,1,2)))
static void log_msg(const char *fmt, ...) {
    rotate_log_if_needed();
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;

    /* timestamp */
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    if (tm) {
        fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ============================================================= */
/* Network helpers – keep bsd:ux alive for entire process      */
/* ============================================================= */

static bool net_is_online(void) {
    if (!g_net_up) return false;
    u32 ip = 0;
    Result rc = nifmGetCurrentIpAddress(&ip);
    return (R_SUCCEEDED(rc) && ip != 0);
}

static void net_cleanup(void) {
    if (g_net_up) {
        http_server_stop();
        nifmExit();
        g_net_up = false;
    }
    /* Do NOT call socketExit() — keep bsd:ux alive */
}

static Result net_init(void) {
    Result rc;

    /*
     * Initialize bsd:ux ONCE for the entire process lifetime.
     * Never call socketExit() until process exit (__appExit).
     */
    if (!g_bsd_initialized) {
        rc = socketInitializeDefault();
        if (R_FAILED(rc)) {
            log_msg("socketInitializeDefault failed: 0x%X", rc);
            return rc;
        }
        g_bsd_initialized = true;
        log_msg("bsd:ux initialized (one-time, process lifetime).");
    }

    /* Initialize nifm service */
    rc = nifmInitialize(NIFM_SERVICE_TYPE_USER);
    if (R_FAILED(rc)) {
        log_msg("nifmInitialize failed: 0x%X", rc);
        return rc;
    }
    g_net_up = true;
    log_msg("nifm initialized.");

    /* Wait for IP address to become available */
    log_msg("Waiting for IP address...");
    u32 ipaddr = 0;
    int waited = 0;
    while (waited < 30) {
        rc = nifmGetCurrentIpAddress(&ipaddr);
        if (R_SUCCEEDED(rc) && ipaddr != 0)
            break;
        svcSleepThread(1000000000ULL);
        waited++;
    }

    if (waited >= 30 || R_FAILED(rc) || ipaddr == 0) {
        log_msg("IP address not available after 30 seconds, rc=0x%X, ip=%08X", rc, ipaddr);
        net_cleanup();
        return MAKERESULT(Module_Custom, 1);
    }

    log_msg("IP address acquired: %d.%d.%d.%d",
            (ipaddr >> 0) & 0xFF,
            (ipaddr >> 8) & 0xFF,
            (ipaddr >> 16) & 0xFF,
            (ipaddr >> 24) & 0xFF);

    /* Start HTTP server */
    rc = http_server_start();
    if (R_FAILED(rc)) {
        log_msg("http_server_start failed: 0x%X", rc);
        net_cleanup();
        return rc;
    }

    log_msg("Network services initialized, HTTP server started.");
    return 0;
}

/* ============================================================= */
/* Main                                                          */
/* ============================================================= */

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    log_msg("pctltcp-sysmodule starting (v1.7d)...");

    /* Load timezone */
    FILE *tz = fopen("sdmc:/switch/pctltcp-sysmodule/tz.txt", "r");
    if (tz) {
        char tzbuf[64] = {0};
        if (fgets(tzbuf, sizeof(tzbuf), tz)) {
            tzbuf[strcspn(tzbuf, "\r\n")] = 0;
            setenv("TZ", tzbuf, 1);
            tzset();
        }
        fclose(tz);
        log_msg("Timezone rule loaded successfully.");
    }

    /* Time service */
    time_t now = time(NULL);
    log_msg("Time service: OK (time=%ld, rc=0x%lX)", now, 0);

    /* Initialize network and HTTP server */
    Result rc = net_init();
    if (R_FAILED(rc)) {
        log_msg("Initial network init failed (rc=0x%lX), will retry in main loop.", rc);
    } else {
        u32 ip = 0;
        nifmGetCurrentIpAddress(&ip);
        log_msg("Web UI: http://%d.%d.%d.%d:%d",
                (ip >> 0) & 0xFF, (ip >> 8) & 0xFF,
                (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
                HTTP_PORT);
    }

    log_msg("pctltcp-sysmodule initialization complete.");

    /* Main loop: proactively monitor network status */
    while (1) {
        svcSleepThread(1000000000ULL);  /* sleep 1 second */

        bool online = net_is_online();

        if (!online) {
            if (g_net_up) {
                log_msg("Network lost (sleep?). Stopping HTTP server.");
                net_cleanup();
            }
        } else {
            if (!g_net_up) {
                log_msg("Network restored, starting HTTP server.");
                net_init();
                if (g_net_up) {
                    u32 ip = 0;
                    nifmGetCurrentIpAddress(&ip);
                    log_msg("Web UI: http://%d.%d.%d.%d:%d",
                            (ip >> 0) & 0xFF, (ip >> 8) & 0xFF,
                            (ip >> 16) & 0xFF, (ip >> 24) & 0xFF,
                            HTTP_PORT);
                }
            }
        }
    }

    /* Unreachable, but keep compiler happy */
    return 0;
}
