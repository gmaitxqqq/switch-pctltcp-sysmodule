/**
 * pctl_handler.h - Nintendo Switch PCTL service wrapper
 *
 * Provides high-level functions for interacting with the Switch
 * parental control (pctl) play timer service.
 *
 * Uses libnx pctlInitialize() + serviceDispatch*() for proper IPC.
 *
 * Sysmodule compatibility:
 *   pctlInitialize() tries pctl:a -> pctl:s -> pctl:r -> pctl
 *   In sysmodule context, pctl:a is likely denied but pctl:s should work.
 *   Read commands work on pctl:s; write commands (195101) may need pctl:a.
 *
 * Based on NX-Pctl-Manager / switch-parental-timer pctl IPC research:
 *   - GetPlayTimerSettings (cmd 145601) returns 0x44 bytes for FW 18.0.0+
 *   - SetPlayTimerSettingsForDebug (cmd 195101) takes 0x44 bytes
 *   - Layout: u16[34], day n at [7+4n] (flag), [7+4n+2] (minutes)
 *   - Day order: Sun=0, Mon=1, ..., Sat=6
 */

#ifndef PCTL_HANDLER_H
#define PCTL_HANDLER_H

#include <switch.h>

#define PCTL_PLAY_TIMER_SETTINGS_SIZE   0x44   /* 68 bytes */
#define PCTL_SETTINGS_U16_COUNT         (PCTL_PLAY_TIMER_SETTINGS_SIZE / 2)  /* 34 */
#define PCTL_DAYS                       7      /* Sun..Sat */
#define PCTL_DAY_FLAG_OFFSET(n)         (7 + 4 * (n))     /* u16 offset */
#define PCTL_DAY_MINUTES_OFFSET(n)      (7 + 4 * (n) + 2) /* u16 offset */
#define PT_DAY_NOLIMIT                  0xFFFFu

/* Play timer settings: raw u16[34] */
#pragma pack(push, 1)
typedef struct {
    u16 raw[PCTL_SETTINGS_U16_COUNT];  /* 68 bytes = 0x44 */
} PlayTimerSettings;
#pragma pack(pop)

_Static_assert(sizeof(PlayTimerSettings) == PCTL_PLAY_TIMER_SETTINGS_SIZE,
    "PlayTimerSettings must be 0x44 bytes");

#define MINUTES_TO_NS(m)  ((u64)(m) * 60ULL * 1000000000ULL)
#define NS_TO_MINUTES(ns) ((u32)((ns) / (60ULL * 1000000000ULL)))

/**
 * Initialize pctl service using libnx pctlInitialize().
 * Works in both .nro and sysmodule context (pctl:s fallback).
 */
Result pctl_init(void);

/**
 * Release all pctl resources.
 */
void pctl_exit(void);

/** Check if pctl has been successfully initialized. */
bool pctl_is_initialized(void);

/** Start the play timer. */
Result pctl_start_play_timer(void);

/** Stop the play timer. */
Result pctl_stop_play_timer(void);

/** Check if play timer is enabled. */
Result pctl_is_enabled(bool *enabled);

/** Get remaining play time in nanoseconds. */
Result pctl_get_remaining_time(u64 *remaining_ns);

/** Check if system is restricted by play timer. */
Result pctl_is_restricted(bool *restricted);

/** Read current play timer settings (raw 0x44 bytes). */
Result pctl_get_settings(PlayTimerSettings *settings);

/** Write play timer settings. */
Result pctl_set_settings(const PlayTimerSettings *settings);

/**
 * Get play time limit for a specific day in minutes.
 * @param day 0=Sun..6=Sat, 7=All (returns max)
 * @param minutes Output: minutes for that day (0=unlimited)
 */
Result pctl_get_day_limit_minutes(int day, u32 *minutes);

/**
 * Set play time limit for a specific day in minutes.
 * @param day 0=Sun..6=Sat
 * @param minutes Time limit (0=unlimited=PT_DAY_NOLIMIT, or 1-1440)
 */
Result pctl_set_day_limit_minutes(int day, u32 minutes);

/**
 * Set all 7 days to the same limit.
 * @param minutes Daily time limit (0=unlimited, 1-1440)
 */
Result pctl_set_daily_limit_minutes(u32 minutes);

/** Get today's day index (0=Sun..6=Sat). */
int pctl_get_today_day(void);

/** Get current daily limit (returns first day's value). */
Result pctl_get_daily_limit_minutes(u32 *minutes);

/** Reset current day's play time (stop + re-apply settings + start). */
Result pctl_reset_play_time(void);

/**
 * Load the system timezone rule for correct day-of-week calculation.
 * MUST be called once after timeInitialize() during startup.
 * This is needed because timeToCalendarTimeWithMyRule() may fail
 * in sysmodule context where the timezone rule isn't auto-loaded.
 */
Result pctl_load_timezone(void);

#endif /* PCTL_HANDLER_H */
