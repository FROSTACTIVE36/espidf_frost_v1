#pragma once

#include <cstddef>
#include <cstdint>


static constexpr std::size_t MAX_POMODORO_LAPS = 12;

struct PomodoroLap
{
    bool enabled = true;

    uint8_t start_hour = 0;
    uint8_t start_minute = 0;

    uint8_t end_hour = 0;
    uint8_t end_minute = 0;
};


/*
 * Counter appearance and position.
 *
 * text_align:
 * 0 = left
 * 1 = center
 * 2 = right
 */
struct PomodoroCounterStyle
{
    int16_t x = 120;
    int16_t y = 150;

    uint8_t text_size = 3;
    uint16_t text_color = 0xFFFF;
    uint8_t text_align = 1;
};

struct PomodoroConfig
{
    bool enabled = false;

    uint16_t focus_min = 25;
    uint16_t break_min = 5;

    uint8_t cycles = 4;

    bool auto_start_break = true;
    bool auto_start_focus = true;

    /*
     * Lap mode automatically runs Pomodoro only inside one of
     * the configured daily time windows.
     */
    bool lap_mode_enabled = false;
    PomodoroLap laps[MAX_POMODORO_LAPS] = {};
    std::size_t lap_count = 0;

    PomodoroCounterStyle focus_counter = {};
    PomodoroCounterStyle break_counter = {};
};

enum class PomodoroState : uint8_t
{
    STOPPED,
    FOCUS,
    BREAK
};

void pomodoro_init();

void pomodoro_set_config(
    const PomodoroConfig& config
);

const PomodoroConfig& pomodoro_get_config();

bool pomodoro_start();
void pomodoro_stop();
void pomodoro_toggle();

void pomodoro_update();

bool pomodoro_is_running();

PomodoroState pomodoro_get_state();

uint8_t pomodoro_get_current_cycle();

uint32_t pomodoro_get_remaining_seconds();

/*
 * Draws only when the second or state changes.
 */
void pomodoro_render_if_needed();

/*
 * Forces a redraw after a reminder has covered the screen.
 */
void pomodoro_force_redraw();

/*
 * Lap-mode status helpers.
 *
 * Active lap index is -1 when the current time is outside every
 * enabled lap window.
 */
bool pomodoro_is_lap_mode_enabled();
int pomodoro_get_active_lap_index();
std::size_t pomodoro_get_lap_count();