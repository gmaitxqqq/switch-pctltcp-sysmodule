#ifndef PCTL_HANDLER_H
#define PCTL_HANDLER_H
#include <switch.h>

#define PCTL_PLAY_TIMER_SETTINGS_SIZE   0x44
#define PCTL_SETTINGS_U16_COUNT         (PCTL_PLAY_TIMER_SETTINGS_SIZE / 2)
#define PCTL_DAYS                       7
#define PCTL_DAY_FLAG_OFFSET(n)         (7 + 4 * (n))
#define PCTL_DAY_MINUTES_OFFSET(n)      (7 + 4 * (n) + 2)
#define PT_DAY_NOLIMIT                  0xFFFFu

#pragma pack(push, 1)
typedef struct { u16 raw[PCTL_SETTINGS_U16_COUNT]; } PlayTimerSettings;
#pragma pack(pop)

_Static_assert(sizeof(PlayTimerSettings) == PCTL_PLAY_TIMER_SETTINGS_SIZE,
    "PlayTimerSettings must be 0x44 bytes");

#define MINUTES_TO_NS(m)  ((u64)(m) * 60ULL * 1000000000ULL)
#define NS_TO_MINUTES(ns) ((u32)((ns) / (60ULL * 1000000000ULL)))

Result pctl_init(void);
void pctl_exit(void);
bool pctl_is_initialized(void);
Result pctl_start_play_timer(void);
Result pctl_stop_play_timer(void);
Result pctl_is_enabled(bool *enabled);
Result pctl_get_remaining_time(u64 *remaining_ns);
Result pctl_is_restricted(bool *restricted);
Result pctl_get_settings(PlayTimerSettings *settings);
Result pctl_set_settings(const PlayTimerSettings *settings);
Result pctl_get_day_limit_minutes(int day, u32 *minutes);
Result pctl_set_day_limit_minutes(int day, u32 minutes);
Result pctl_set_daily_limit_minutes(u32 minutes);
Result pctl_get_daily_limit_minutes(u32 *minutes);
Result pctl_reset_play_time(void);

#endif
