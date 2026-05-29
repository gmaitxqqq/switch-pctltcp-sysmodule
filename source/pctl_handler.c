/**
 * pctl_handler.c - Nintendo Switch PCTL service IPC wrapper
 *
 * Based on switch-parental-timer v11.5 source/main.c
 * (which successfully calls pctl IPC on .nro context).
 *
 * IPC command IDs (verified working on fw 22.1.0):
 *   1451:   StartPlayTimer
 *   1452:   StopPlayTimer
 *   1453:   IsPlayTimerEnabled          (out: bool, inline)
 *   1454:   GetPlayTimerRemainingTime  (out: u64, inline)
 *   1455:   IsRestrictedByPlayTimer   (out: bool, inline)
 *   145601: GetPlayTimerSettings       (out: u16[34], pointer buffer)
 *   195101: SetPlayTimerSettingsForDebug (in:  u16[34], pointer buffer)
 *
 * PlayTimerSettings layout: u16[34] (0x44 bytes)
 *   [0]      header magic  (0x0101 when days are set)
 *   [1]      header flag   (0x0001 when enabled)
 *   [2-6]    reserved      (zero)
 *   Day n (Sun=0 .. Sat=6):
 *     [7+4n+0]  day flag     (0x0600 = configured)
 *     [7+4n+1]  day enable   (0x0100 = restricted, 0x0000 = skip)
 *     [7+4n+2]  day minutes  (0=blocked, 1-1440=limit, 0xFFFF=unlimited)
 *     [7+4n+3]  day padding  (zero)
 */

#include "pctl_handler.h"
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* Global state                                                        */
/* ------------------------------------------------------------------ */
static Service s_pctlSrv;
static bool s_initialized = false;

/* ------------------------------------------------------------------ */
/* pctl_init: Use libnx pctlInitialize() for proper service setup      */
/* ------------------------------------------------------------------ */
Result pctl_init(void)
{
    Result rc;

    if (s_initialized)
        return 0;

    rc = pctlInitialize();
    if (R_FAILED(rc))
        return rc;

    Service *srv = pctlGetServiceSession_Service();
    if (srv == NULL) {
        pctlExit();
        return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    }
    s_pctlSrv = *srv;
    s_initialized = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* pctl_exit                                                           */
/* ------------------------------------------------------------------ */
void pctl_exit(void)
{
    if (!s_initialized)
        return;

    pctlExit();
    s_initialized = false;
}

bool pctl_is_initialized(void)
{
    return s_initialized;
}

/* ------------------------------------------------------------------ */
/* Re-initialize pctl session (needed between certain calls)           */
/* Based on switch-parental-timer v11.5 pctl_ops_reinit()          */
/* ------------------------------------------------------------------ */
static Result pctl_reinit(void)
{
    pctl_exit();
    return pctl_init();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * pctl_start_play_timer / pctl_stop_play_timer:
 *   Do NOT call pctl_reinit() — reinit would reset the pctl
 *   service state and the timer would not actually start/stop.
 *   Based on v11.5: it never reinit() for read/control commands.
 */
Result pctl_start_play_timer(void)
{
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatch(&s_pctlSrv, 1451);
}

Result pctl_stop_play_timer(void)
{
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatch(&s_pctlSrv, 1452);
}

Result pctl_is_enabled(bool *enabled)
{
    if (!enabled) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u8 tmp = 0;
    Result rc = serviceDispatchOut(&s_pctlSrv, 1453, tmp);
    if (R_SUCCEEDED(rc))
        *enabled = (tmp != 0);
    return rc;
}

Result pctl_get_remaining_time(u64 *remaining_ns)
{
    if (!remaining_ns) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u64 tmp = 0;
    Result rc = serviceDispatchOut(&s_pctlSrv, 1454, tmp);
    if (R_SUCCEEDED(rc))
        *remaining_ns = tmp;
    return rc;
}

Result pctl_is_restricted(bool *restricted)
{
    if (!restricted) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u8 tmp = 0;
    Result rc = serviceDispatchOut(&s_pctlSrv, 1455, tmp);
    if (R_SUCCEEDED(rc))
        *restricted = (tmp != 0);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Settings read/write                                                  */
/* Based on switch-parental-timer v11.5 pctl_play_timer_query()    */
/* and pctl_play_timer_set_days()                                      */
/* ------------------------------------------------------------------ */

Result pctl_get_settings(PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(settings, 0, sizeof(*settings));

    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    /* GetPlayTimerSettings (cmd 145601):
     * Output: u16[34] as inline parameter.
     * Based on v11.5 line 178: serviceDispatchOut(srv, 145601, c)
     * where c is u16[34]. Pure inline, NO buffer_attrs/buffers!
     * Do NOT reinit() — v11.5 never does this for read commands. */
    u16 c[34];
    memset(c, 0, sizeof(c));

    Service *srv = pctlGetServiceSession_Service();
    if (!srv) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = serviceDispatchOut(srv, 145601, c);
    if (R_SUCCEEDED(rc)) {
        memcpy(settings, c, sizeof(c));
    }
    return rc;
}

Result pctl_set_settings(const PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    /* SetPlayTimerSettingsForDebug (cmd 195101):
     * Input: u16[34] as inline parameter.
     * Based on v11.5 line 204:
     *   serviceDispatchIn(pctlGetServiceSession_Service(), 195101, c)
     * where c is u16[34]. Pure inline, NO buffer_attrs/buffers! */
    u16 c[34];
    memcpy(c, settings->raw, sizeof(c));

    Service *srv = pctlGetServiceSession_Service();
    if (!srv) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatchIn(srv, 195101, c);
}

/* ------------------------------------------------------------------ */
/* Day-aware convenience functions                                     */
/* ------------------------------------------------------------------ */

Result pctl_get_day_limit_minutes(int day, u32 *minutes)
{
    if (!minutes || day < 0 || day > 7)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    if (day == 7) {
        /* Return max minutes across all days */
        *minutes = 0;
        for (int d = 0; d < PCTL_DAYS; d++) {
            u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(d)];
            if (m == PT_DAY_NOLIMIT) {
                *minutes = 0;  /* any day unlimited => report 0 */
                return 0;
            }
            if (m > *minutes) *minutes = m;
        }
    } else {
        u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(day)];
        *minutes = (m == PT_DAY_NOLIMIT) ? 0 : m;
    }
    return 0;
}

Result pctl_set_day_limit_minutes(int day, u32 minutes)
{
    if (day < 0 || day >= PCTL_DAYS)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);

    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    u16 val;
    if (minutes == 0) {
        val = PT_DAY_NOLIMIT;
    } else {
        if (minutes > 1440) minutes = 1440;
        val = (u16)minutes;
    }

    /* Set header if not already set */
    if (settings.raw[0] == 0) {
        settings.raw[0] = 0x0101;
        settings.raw[1] = 0x0001;
    }

    /* Set day flag + minutes */
    settings.raw[PCTL_DAY_FLAG_OFFSET(day)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
    settings.raw[PCTL_DAY_MINUTES_OFFSET(day)] = val;

    return pctl_set_settings(&settings);
}

Result pctl_set_daily_limit_minutes(u32 minutes)
{
    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    u16 val;
    if (minutes == 0) {
        val = PT_DAY_NOLIMIT;
    } else {
        if (minutes > 1440) minutes = 1440;
        val = (u16)minutes;
    }

    /* Set header */
    settings.raw[0] = (val != PT_DAY_NOLIMIT) ? 0x0101 : 0x0000;
    settings.raw[1] = (val != PT_DAY_NOLIMIT) ? 0x0001 : 0x0000;

    for (int d = 0; d < PCTL_DAYS; d++) {
        settings.raw[PCTL_DAY_FLAG_OFFSET(d)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
        settings.raw[PCTL_DAY_MINUTES_OFFSET(d)] = val;
    }

    return pctl_set_settings(&settings);
}

/**
 * Get today's day-of-week in Switch convention: 0=Sun, 1=Mon, ..., 6=Sat.
 * Falls back to 0 (Sun) on error.
 */
int pctl_get_today_day(void)
{
    time_t t = time(NULL);
    if (t == (time_t)-1)
        return 0;
    struct tm *tm_info = localtime(&t);
    if (!tm_info)
        return 0;
    /* tm_wday: 0=Sun, 1=Mon, ..., 6=Sat — same as Switch day convention */
    return tm_info->tm_wday;
}

Result pctl_get_daily_limit_minutes(u32 *minutes)
{
    /* Read today's limit, not a hardcoded day=0 (Sun) */
    int today = pctl_get_today_day();
    return pctl_get_day_limit_minutes(today, minutes);
}

Result pctl_reset_play_time(void)
{
    Result rc;

    /* Step 1: Stop the play timer */
    rc = pctl_stop_play_timer();
    if (R_FAILED(rc)) return rc;

    /* Step 2: Get current settings */
    PlayTimerSettings settings;
    rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;

    /* Step 3: Re-apply the same settings — this resets the internal
     * play time counter for today, restoring remaining time to the limit */
    rc = pctl_set_settings(&settings);
    if (R_FAILED(rc)) return rc;

    /* Step 4: Restart the play timer */
    rc = pctl_start_play_timer();
    if (R_FAILED(rc)) return rc;

    return 0;
}
