#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>

#include "esp_err.h"
#include "reminder_types.hpp"

static constexpr std::size_t MAX_AUDIO_PLAYLIST_TRACKS = 16;
static constexpr std::size_t MAX_HEALING_SCHEDULES = 12;

struct AudioPlaylistConfig
{
    bool enabled = true;
    uint16_t tracks[MAX_AUDIO_PLAYLIST_TRACKS] = {};
    std::size_t track_count = 0;
};

struct HealingScheduleConfig
{
    bool enabled = false;

    // Bit 0=Sun ... bit 6=Sat. Zero means every day.
    uint8_t day_mask = 0;

    uint8_t start_hour = 0;
    uint8_t start_minute = 0;

    uint8_t end_hour = 0;
    uint8_t end_minute = 0;
};

struct AudioManagerConfig
{
    uint8_t volume = 20;
    AudioPlaylistConfig pomodoro = {};
    AudioPlaylistConfig meditation = {};
    AudioPlaylistConfig healing = {};

    // When true, healing runs only while the IR sensor reports docked.
    bool healing_require_dock = false;

    HealingScheduleConfig healing_schedules[MAX_HEALING_SCHEDULES] = {};
    std::size_t healing_schedule_count = 0;
};

enum class AudioBackgroundMode : uint8_t
{
    NONE,
    POMODORO,
    MEDITATION,
    HEALING
};

esp_err_t audio_manager_init();
void audio_manager_update(time_t now);

void audio_manager_set_config(
    const AudioManagerConfig& config
);

const AudioManagerConfig& audio_manager_get_config();

void audio_manager_start_pomodoro();
void audio_manager_start_pomodoro_focus();
void audio_manager_start_pomodoro_break();
void audio_manager_stop_pomodoro();

void audio_manager_start_meditation();
void audio_manager_stop_meditation();

void audio_manager_start_healing();
void audio_manager_stop_healing();
void audio_manager_set_dock_state(bool docked);
bool audio_manager_is_docked();
void audio_manager_play_welcome();
void audio_manager_play_reminder(ReminderType type);

AudioBackgroundMode audio_manager_get_background_mode();