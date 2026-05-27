#include "pctl_handler.h"
#include <string.h>
#include <time.h>

static Service s_pctlSrv;
static bool s_initialized = false;

Result pctl_init(void)
{
    if (s_initialized)
        return 0;

    Result rc = pctlInitialize();
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

static Result pctl_reinit(void)
{
    pctl_exit();
    return pctl_init();
}

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

Result pctl_get_settings(PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    memset(settings, 0, sizeof(*settings));
    if (!s_initialized) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

    u16 c[34];
    memset(c, 0, sizeof(c));
    Service *srv = pctlGetServiceSession_Service();
    if (!srv) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    Result rc = serviceDispatchOut(srv, 145601, c);
    if (R_SUCCEEDED(rc))
        memcpy(settings, c, sizeof(c));
    return rc;
}

Result pctl_set_settings(const PlayTimerSettings *settings)
{
    if (!settings) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    Result rc = pctl_reinit();
    if (R_FAILED(rc)) return rc;

    u16 c[34];
    memcpy(c, settings->raw, sizeof(c));
    Service *srv = pctlGetServiceSession_Service();
    if (!srv) return MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
    return serviceDispatchIn(srv, 195101, c);
}

Result pctl_get_day_limit_minutes(int day, u32 *minutes)
{
    if (!minutes || day < 0 || day > 7)
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;
    if (day == 7) {
        *minutes = 0;
        for (int d = 0; d < PCTL_DAYS; d++) {
            u16 m = settings.raw[PCTL_DAY_MINUTES_OFFSET(d)];
            if (m == PT_DAY_NOLIMIT) { *minutes = 0; return 0; }
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
    u16 val = (minutes == 0) ? PT_DAY_NOLIMIT : ((minutes > 1440) ? 1440 : (u16)minutes);
    if (settings.raw[0] == 0) { settings.raw[0] = 0x0101; settings.raw[1] = 0x0001; }
    settings.raw[PCTL_DAY_FLAG_OFFSET(day)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
    settings.raw[PCTL_DAY_MINUTES_OFFSET(day)] = val;
    return pctl_set_settings(&settings);
}

Result pctl_set_daily_limit_minutes(u32 minutes)
{
    PlayTimerSettings settings;
    Result rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;
    u16 val = (minutes == 0) ? PT_DAY_NOLIMIT : ((minutes > 1440) ? 1440 : (u16)minutes);
    settings.raw[0] = (val != PT_DAY_NOLIMIT) ? 0x0101 : 0x0000;
    settings.raw[1] = (val != PT_DAY_NOLIMIT) ? 0x0001 : 0x0000;
    for (int d = 0; d < PCTL_DAYS; d++) {
        settings.raw[PCTL_DAY_FLAG_OFFSET(d)]    = (val != PT_DAY_NOLIMIT) ? 0x0100 : 0x0000;
        settings.raw[PCTL_DAY_MINUTES_OFFSET(d)] = val;
    }
    return pctl_set_settings(&settings);
}

static int get_today_switch_day(void)
{
    time_t t = time(NULL);
    if (t == (time_t)-1) return 0;
    struct tm *tm_info = localtime(&t);
    if (!tm_info) return 0;
    return tm_info->tm_wday;  /* 0=Sun..6=Sat */
}

Result pctl_get_daily_limit_minutes(u32 *minutes)
{
    int today = get_today_switch_day();
    return pctl_get_day_limit_minutes(today, minutes);
}

Result pctl_reset_play_time(void)
{
    Result rc;
    rc = pctl_stop_play_timer();
    if (R_FAILED(rc)) return rc;
    PlayTimerSettings settings;
    rc = pctl_get_settings(&settings);
    if (R_FAILED(rc)) return rc;
    rc = pctl_set_settings(&settings);
    if (R_FAILED(rc)) return rc;
    return pctl_start_play_timer();
}
