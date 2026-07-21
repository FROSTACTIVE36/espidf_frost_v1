#pragma once

#include <cstdint>
#include <ctime>
#include "pomodoro.hpp"

bool display_init();

void display_clear(
    uint16_t color
);

void display_show_frost_logo();

void display_show_hydration_reminder();
void display_show_stretch_reminder();
void display_show_eye_reminder();
void display_show_walk_reminder();

void display_show_meditation_reminder();

void display_show_medication_reminder(
    const char* label,
    int16_t text_x,
    int16_t text_y,
    uint8_t text_size,
    uint16_t text_color,
    uint8_t text_align,
    uint16_t text_width
);

void display_show_custom_reminder(
    const char* label,
    int16_t text_x,
    int16_t text_y,
    uint8_t text_size,
    uint16_t text_color,
    uint8_t text_align,
    uint16_t text_width
);

void display_show_home_clock(
    time_t current_time
);
void display_show_pomodoro_focus(
    uint32_t remaining_seconds,
    uint32_t total_seconds,
    const PomodoroCounterStyle& style
);

void display_show_pomodoro_break(
    uint32_t remaining_seconds,
    uint32_t total_seconds,
    const PomodoroCounterStyle& style
);