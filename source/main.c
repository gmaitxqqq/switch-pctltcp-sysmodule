#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

#include "pctl_handler.h"
#include "http_server.h"

/* Sysmodule = no application framework */
u32 __nx_applet_type = AppletType_None;

static u8 _heap[0x100000];
void __libnx_initheap(void)
{
    extern void *fake_heap_start;
    extern void *fake_heap_end;
    fake_heap_start = _heap;
    fake_heap_end   = _heap + sizeof(_heap);
}

void __appInit(void)
{
    smInitialize();
    setsysInitialize();
    /* socketInit is done in main() with retry */
}

void __appExit(void)
{
    setsysExit();
    smExit();
}

/* ── main ── */
int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    /* 1. Init pctl (best-effort) */
    Result pctl_rc = pctl_init();
    /* Ignore error - we still run HTTP server */

    /* 2. Init socket with retry (network may not be up yet) */
    Result sock_rc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    for (int i = 0; i < 60 && R_FAILED(sock_rc); i++) {
        sock_rc = socketInitializeDefault();
        if (R_SUCCEEDED(sock_rc)) break;
        svcSleepThread(2000000000ULL);
    }

    if (R_FAILED(sock_rc)) {
        if (R_SUCCEEDED(pctl_rc)) pctl_exit();
        return 1;
    }

    /* 3. Start HTTP server thread */
    http_server_start();

    /* 4. Main loop - sleep until system asks us to exit */
    while (1) {
        svcSleepThread(1000000000ULL);  /* 1 s */
        if (!http_server_is_running())
            break;
    }

    /* 5. Cleanup */
    http_server_stop();
    if (R_SUCCEEDED(pctl_rc)) pctl_exit();
    socketExit();
    return 0;
}
