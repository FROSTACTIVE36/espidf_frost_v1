#include "pomodoro.hpp"

#include <algorithm>
#include <cstdint>
#include <ctime>

#include "esp_log.h"
#include "esp_timer.h"

#include "display.hpp"
#include "reminder_engine.hpp"
#include "audio_manager.hpp"

static const char* TAG = "POMODORO";

/* =========================================================
 * Configuration and runtime state
 * ========================================================= */

static PomodoroConfig pomodoro_config;

static PomodoroState pomodoro_state =
    PomodoroState::STOPPED;

static bool pomodoro_running = false;

static uint8_t current_cycle = 0;

static uint64_t stage_end_us = 0;

static uint32_t remaining_seconds = 0;

/*
 * Full duration of the currently active focus or break stage.
 * The display uses this to calculate the progressive arc.
 */
static uint32_t stage_total_seconds = 0;

/*
 * Display caching prevents four redraws per second because the
 * main loop currently runs every 250 ms.
 */
static uint32_t last_rendered_seconds =
    UINT32_MAX;

static PomodoroState last_rendered_state =
    PomodoroState::STOPPED;

static bool redraw_required = false;

/* -1 means no lap session is running. */
static int active_lap_index = -1;
static int last_started_date[MAX_POMODORO_LAPS] = {};

static int calendar_date(const std::tm& clock)
{
    return (clock.tm_year + 1900) * 10000 +
           (clock.tm_mon + 1) * 100 + clock.tm_mday;
}

static int configured_date(const ReminderDate& date)
{
    return date.year * 10000 + date.month * 100 + date.day;
}

static bool scheduled_day(const std::tm& clock)
{
    const int today = calendar_date(clock);
    return today >= configured_date(pomodoro_config.start_date) &&
           today <= configured_date(pomodoro_config.end_date) &&
           (pomodoro_config.day_mask == 0 ||
            (pomodoro_config.day_mask & (1U << clock.tm_wday)) != 0);
}

static uint64_t current_time_us()
{
    return static_cast<uint64_t>(
        esp_timer_get_time()
    );
}

static uint32_t minutes_to_seconds(
    uint16_t minutes
)
{
    /*
     * Keep at least one second so a zero configuration cannot
     * create an endless immediate transition.
     */
    return std::max<uint32_t>(
        1U,
        static_cast<uint32_t>(minutes) * 60U
    );
}

static void begin_stage(
    PomodoroState new_state,
    uint32_t duration_seconds
)
{
    pomodoro_state = new_state;

    remaining_seconds = duration_seconds;
    stage_total_seconds = duration_seconds;

    stage_end_us =
        current_time_us() +
        static_cast<uint64_t>(
            duration_seconds
        ) * 1000000ULL;

    redraw_required = true;

    last_rendered_seconds =
        UINT32_MAX;

    last_rendered_state =
        PomodoroState::STOPPED;

    ESP_LOGI(
        TAG,
        "%s started: cycle=%u/%u duration=%lu seconds",
        new_state == PomodoroState::FOCUS
            ? "Focus"
            : "Break",
        static_cast<unsigned>(
            current_cycle
        ),
        static_cast<unsigned>(
            pomodoro_config.laps[active_lap_index].cycles
        ),
        static_cast<unsigned long>(
            duration_seconds
        )
    );

    /*
     * Play the stage announcement first. The audio manager waits
     * for the DFPlayer BUSY pin to go LOW and then HIGH before
     * starting the configured Pomodoro background playlist.
     */
    if (new_state == PomodoroState::FOCUS)
    {
        audio_manager_start_pomodoro_focus();
    }
    else if (new_state == PomodoroState::BREAK)
    {
        audio_manager_start_pomodoro_break();
    }
}

/* =========================================================
 * Public API
 * ========================================================= */

void pomodoro_init()
{
    pomodoro_state =
        PomodoroState::STOPPED;

    pomodoro_running = false;

    current_cycle = 0;
    stage_end_us = 0;
    remaining_seconds = 0;
    stage_total_seconds = 0;

    redraw_required = false;
    active_lap_index = -1;
    std::fill_n(last_started_date, MAX_POMODORO_LAPS, 0);

    last_rendered_seconds =
        UINT32_MAX;

    last_rendered_state =
        PomodoroState::STOPPED;

    ESP_LOGI(
        TAG,
        "Pomodoro initialized"
    );
}

void pomodoro_set_config(
    const PomodoroConfig& new_config
)
{
    pomodoro_config =
        new_config;

    if (pomodoro_config.focus_min == 0)
    {
        pomodoro_config.focus_min = 1;
    }

    if (pomodoro_config.break_min == 0)
    {
        pomodoro_config.break_min = 1;
    }

    if (
        pomodoro_config.lap_count >
        MAX_POMODORO_LAPS
    )
    {
        pomodoro_config.lap_count =
            MAX_POMODORO_LAPS;
    }

    for (
        std::size_t index = 0;
        index < pomodoro_config.lap_count;
        ++index
    )
    {
        PomodoroLap& lap =
            pomodoro_config.laps[index];

        if (lap.start_hour > 23)
        {
            lap.start_hour = 23;
        }

        if (lap.start_minute > 59)
        {
            lap.start_minute = 59;
        }
        if (lap.cycles == 0)
        {
            lap.cycles = 1;
        }
    }

    pomodoro_stop();
    active_lap_index = -1;
    std::fill_n(last_started_date, MAX_POMODORO_LAPS, 0);

    ESP_LOGI(
        TAG,
        "Config: enabled=%d focus=%u break=%u laps=%u",
        pomodoro_config.enabled,
        static_cast<unsigned>(
            pomodoro_config.focus_min
        ),
        static_cast<unsigned>(
            pomodoro_config.break_min
        ),
        static_cast<unsigned>(
            pomodoro_config.lap_count
        )
    );
}

const PomodoroConfig& pomodoro_get_config()
{
    return pomodoro_config;
}

bool pomodoro_start()
{
    if (!pomodoro_config.enabled || active_lap_index < 0 ||
        static_cast<std::size_t>(active_lap_index) >= pomodoro_config.lap_count)
    {
        ESP_LOGW(
            TAG,
            "Pomodoro start ignored without an active lap"
        );

        return false;
    }

    pomodoro_running = true;
    current_cycle = 1;

    begin_stage(
        PomodoroState::FOCUS,
        minutes_to_seconds(
            pomodoro_config.focus_min
        )
    );

    return true;
}

void pomodoro_stop()
{
    if (pomodoro_running)
    {
        ESP_LOGI(
            TAG,
            "Pomodoro stopped"
        );
    }

    /*
     * Stop Pomodoro announcement/background playback. If a healing
     * schedule is active, audio_manager_update() can resume healing.
     */
    audio_manager_stop_pomodoro();

    pomodoro_running = false;

    pomodoro_state =
        PomodoroState::STOPPED;

    current_cycle = 0;
    stage_end_us = 0;
    remaining_seconds = 0;
    stage_total_seconds = 0;

    redraw_required = false;
    active_lap_index = -1;

    last_rendered_seconds =
        UINT32_MAX;

    last_rendered_state =
        PomodoroState::STOPPED;
}

void pomodoro_toggle()
{
    /*
     * Manual IR/double-tap control must obey the JSON "enabled" flag.
     * When disabled, ignore the gesture completely and make sure no
     * Pomodoro audio remains active.
     */
    if (!pomodoro_config.enabled)
    {
        ESP_LOGI(
            TAG,
            "Pomodoro toggle ignored because it is disabled"
        );

        audio_manager_stop_pomodoro();
        return;
    }

    if (pomodoro_running)
    {
        pomodoro_stop();
    }
    else
    {
        pomodoro_update();
    }
}

void pomodoro_update()
{
    /*
     * Disabled means fully disabled for both manual and scheduled paths.
     * This guard also self-recovers any inconsistent runtime/audio state.
     */
    if (!pomodoro_config.enabled)
    {
        active_lap_index = -1;

        if (pomodoro_running)
        {
            pomodoro_stop();
        }

        return;
    }

    // Each lap starts once at its exact local clock minute on an allowed day.
    const std::time_t wall_time = std::time(nullptr);
    std::tm clock = {};
    if (localtime_r(&wall_time, &clock) != nullptr && scheduled_day(clock))
    {
        const int today = calendar_date(clock);
        for (std::size_t index = 0; index < pomodoro_config.lap_count; ++index)
        {
            const PomodoroLap& lap = pomodoro_config.laps[index];
            if (clock.tm_hour != lap.start_hour ||
                clock.tm_min != lap.start_minute ||
                last_started_date[index] == today)
            {
                continue;
            }

            // A scheduled start is consumed even if a prior lap is still active.
            last_started_date[index] = today;
            if (!pomodoro_running)
            {
                active_lap_index = static_cast<int>(index);
                pomodoro_start();
            }
            else
            {
                ESP_LOGW(TAG, "Skipping overlapping Pomodoro lap %u",
                         static_cast<unsigned>(index));
            }
        }
    }

    if (!pomodoro_running)
    {
        return;
    }

    const uint64_t now_us =
        current_time_us();

    if (now_us < stage_end_us)
    {
        const uint64_t remaining_us =
            stage_end_us - now_us;

        remaining_seconds =
            static_cast<uint32_t>(
                (
                    remaining_us +
                    999999ULL
                ) /
                1000000ULL
            );

        return;
    }

    remaining_seconds = 0;

    if (
        pomodoro_state ==
        PomodoroState::FOCUS
    )
    {
        begin_stage(
            PomodoroState::BREAK,
            minutes_to_seconds(pomodoro_config.break_min)
        );
        return;
    }

    if (
        pomodoro_state ==
        PomodoroState::BREAK
    )
    {
        if (
            reminder_engine_has_active_reminder()
        )
        {
            remaining_seconds = 0;
            redraw_required = true;

            ESP_LOGD(
                TAG,
                "Break complete; waiting for active reminder to finish"
            );

            return;
        }

        if (current_cycle >= pomodoro_config.laps[active_lap_index].cycles)
        {
            ESP_LOGI(TAG, "Pomodoro lap completed after %u cycles",
                     static_cast<unsigned>(current_cycle));
            pomodoro_stop();
        }
        else
        {
            ++current_cycle;
            begin_stage(PomodoroState::FOCUS,
                        minutes_to_seconds(pomodoro_config.focus_min));
        }

        return;
    }

    ESP_LOGW(
        TAG,
        "Invalid Pomodoro state; stopping session"
    );

    pomodoro_stop();
}

bool pomodoro_is_running()
{
    return pomodoro_running;
}

PomodoroState pomodoro_get_state()
{
    return pomodoro_state;
}

uint8_t pomodoro_get_current_cycle()
{
    return current_cycle;
}

uint32_t pomodoro_get_remaining_seconds()
{
    return remaining_seconds;
}

void pomodoro_render_if_needed()
{
    if (!pomodoro_running)
    {
        return;
    }

    /*
     * Do not redraw when the state and remaining time have
     * not changed.
     */
    if (
        !redraw_required &&
        pomodoro_state ==
            last_rendered_state &&
        remaining_seconds ==
            last_rendered_seconds
    )
    {
        return;
    }

    if (
        pomodoro_state ==
        PomodoroState::FOCUS
    )
    {
        display_show_pomodoro_focus(
            remaining_seconds,
            stage_total_seconds,
            pomodoro_config.focus_counter
        );
    }
    else if (
        pomodoro_state ==
        PomodoroState::BREAK
    )
    {
        display_show_pomodoro_break(
            remaining_seconds,
            stage_total_seconds,
            pomodoro_config.break_counter
        );
    }

    last_rendered_state =
        pomodoro_state;

    last_rendered_seconds =
        remaining_seconds;

    redraw_required = false;
}

void pomodoro_force_redraw()
{
    if (!pomodoro_running)
    {
        return;
    }

    redraw_required = true;

    last_rendered_seconds =
        UINT32_MAX;

    last_rendered_state =
        PomodoroState::STOPPED;
}

bool pomodoro_is_lap_mode_enabled()
{
    return true;
}

int pomodoro_get_active_lap_index()
{
    return active_lap_index;
}

std::size_t pomodoro_get_lap_count()
{
    return pomodoro_config.lap_count;
}
