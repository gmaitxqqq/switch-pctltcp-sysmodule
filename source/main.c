/**
 * switch-pctltcp-sysmodule - Background HTTP server sysmodule
 *
 * Features:
 *   - Runs as Atmosphere sysmodule (boot2.flag)
 *   - Opens HTTP server on port 8080
 *   - Exposes /api/status and /api/set for parental control
 *   - No console output (background service)
 *
 * Sysmodule framework based on sys-botbase pattern.
 *
 * Build: make -> produces pctltcp-sysmodule.nro
 * Deploy: atmosphere/contents/<program_id>/exefs.nsp + flags/boot2.flag
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "pctl_handler.h"
#include "http_server.h"

/* ------------------------------------------------------------------ */
/* Sysmodule entry points                                              */
/* ------------------------------------------------------------------ */
u32 __nx_applet_type = AppletType_None;

/* Manual heap init for sysmodule */
void __libnx_initheap(void)
{
    static u8 _heap[0x100000];  /* 1MB heap */
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = _heap;
    fake_heap_end   = _heap + sizeof(_heap);
}

void __appInit(void)
{
    smInitialize();
    setsysInitialize();
    /* Do NOT call socketInitializeDefault() here.
     * Network service may not be ready at sysmodule init time.
     * We initialize sockets in main() with retry instead. */
}

void __appExit(void)
{
    setsysExit();
    smExit();
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* Initialize pctl service (try pctl:a -> pctl:s -> pctl:r -> pctl) */
    Result pctl_rc = pctl_init();
    /* Ignore error - we still run HTTP server even if pctl fails */

    /* Initialize socket with retry (network may not be ready yet) */
    Result sock_rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    for (int i = 0; i < 60; i++) {
        sock_rc = socketInitializeDefault();
        if (R_SUCCEEDED(sock_rc))
            break;
        svcSleepThread(2000000000ULL);  /* wait 2 seconds */
    }

    if (R_FAILED(sock_rc)) {
        /* Can't start HTTP server without network */
        if (R_SUCCEEDED(pctl_rc)) pctl_exit();
        return 1;
    }

    /* Start HTTP server */
    http_server_start();

    /* Main loop - sleep until system requests exit */
    /* In sysmodule context, we check s_running flag + yield CPU */
    while (1) {
        /* Check if we should exit (system signal) */
        /* sysmodule doesn't have appletMainLoop, use svcSleepThread */
        svcSleepThread(1000000000ULL);  /* sleep 1s */

        /* If HTTP server stopped (error), exit */
        if (!http_server_is_running())
            break;
    }

    /* Cleanup */
    http_server_stop();

    if (R_SUCCEEDED(pctl_rc)) pctl_exit();
    socketExit();

    return 0;
}
