#include "audio_manager.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>

#include "dfplayer.hpp"
#include "esp_log.h"
#include "esp_timer.h"

namespace
{
const char* TAG = "AUDIO_MANAGER";

/*
 * These defaults are used only when JSON has not supplied playlists.
 * JSON can replace both playlists at runtime.
 */
AudioManagerConfig audio_config = {
    .volume = 20,
    .pomodoro = {
        .enabled = true,
        .tracks = {50, 51, 52},
        .track_count = 3
    },
    .meditation = {
        .enabled = true,
        .tracks = {22},
        .track_count = 1
    },
    .healing = {
        .enabled = true,
        .tracks = {60, 61, 62},
        .track_count = 3
    },
    .healing_schedules = {
        {
            .enabled = false,
            .start_hour = 6,
            .start_minute = 0,
            .end_hour = 7,
            .end_minute = 0
        }
    },
    .healing_schedule_count = 1
};

/*
 * Reminder announcement tracks remain fixed for this phase.
 * We can move them into JSON in the next phase.
 */
constexpr uint16_t TONE_WELCOME      = 1;   // /mp3/0001.mp3
constexpr uint16_t TONE_HYDRATION_1  = 11;  // /mp3/0011.mp3
constexpr uint16_t TONE_HYDRATION_2  = 12;  // /mp3/0012.mp3
constexpr uint16_t TONE_EYE          = 13;  // /mp3/0013.mp3
constexpr uint16_t TONE_STRETCH      = 14;  // /mp3/0014.mp3
constexpr uint16_t TONE_MEDICATION   = 15;  // /mp3/0015.mp3
constexpr uint16_t TONE_CLEAN_SOFT   = 16;  // /mp3/0016.mp3
constexpr uint16_t TONE_CLEAN_HARD   = 17;  // /mp3/0017.mp3
constexpr uint16_t TONE_HEALING      = 18;  // /mp3/0018.mp3
constexpr uint16_t TONE_WALK         = 19;  // /mp3/0019.mp3
constexpr uint16_t TONE_POMO_FOCUS   = 20;  // /mp3/0020.mp3
constexpr uint16_t TONE_POMO_BREAK   = 21;  // /mp3/0021.mp3
constexpr uint16_t TONE_MEDITATION   = 22;  // /mp3/0022.mp3
constexpr uint16_t TONE_CUSTOM       = 23;  // /mp3/0023.mp3
constexpr uint16_t TONE_PLACE_BOTTLE = 24;  // /mp3/0024.mp3
constexpr uint16_t TONE_ACK          = 30;  // /mp3/0030.mp3

AudioBackgroundMode background_mode =
    AudioBackgroundMode::NONE;

/* Background mode temporarily paused by meditation. */
AudioBackgroundMode mode_before_meditation =
    AudioBackgroundMode::NONE;

std::size_t playlist_index = 0;
bool waiting_for_track_start = false;
bool saw_track_playing = false;
int64_t command_time_ms = 0;
int64_t idle_since_ms = 0;

/*
 * Pomodoro and healing first play a spoken announcement.
 * The configured background playlist starts only after BUSY has
 * gone LOW (announcement started) and then HIGH (announcement ended).
 */
bool background_announcement_pending = false;
bool announcement_saw_busy_low = false;
uint16_t pending_announcement_track = 0;
int64_t announcement_command_ms = 0;

void stop_background(AudioBackgroundMode mode);

int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

const AudioPlaylistConfig* active_playlist()
{
    switch (background_mode)
    {
        case AudioBackgroundMode::POMODORO:
            return &audio_config.pomodoro;

        case AudioBackgroundMode::MEDITATION:
            return &audio_config.meditation;

        case AudioBackgroundMode::HEALING:
            return &audio_config.healing;

        case AudioBackgroundMode::NONE:
        default:
            return nullptr;
    }
}

uint16_t reminder_track(ReminderType type)
{
    switch (type)
    {
        case ReminderType::HYDRATION:
            return TONE_HYDRATION_1;

        case ReminderType::STRETCH:
            return TONE_STRETCH;

        case ReminderType::EYE:
            return TONE_EYE;

        case ReminderType::WALK:
            return TONE_WALK;

        case ReminderType::MEDITATION:
            return TONE_MEDITATION;

        case ReminderType::MEDICATION:
            return TONE_MEDICATION;

        default:
            return TONE_CUSTOM;
    }
}

uint16_t current_background_track()
{
    const AudioPlaylistConfig* playlist =
        active_playlist();

    if (
        playlist == nullptr ||
        !playlist->enabled ||
        playlist->track_count == 0
    )
    {
        return 0;
    }

    return playlist->tracks[
        playlist_index % playlist->track_count
    ];
}

void reset_track_detection()
{
    waiting_for_track_start = false;
    saw_track_playing = false;
    command_time_ms = 0;
    idle_since_ms = 0;
}

void reset_announcement_detection()
{
    background_announcement_pending = false;
    announcement_saw_busy_low = false;
    pending_announcement_track = 0;
    announcement_command_ms = 0;
}

void play_current_background_track()
{
    const uint16_t track =
        current_background_track();

    if (track == 0)
    {
        ESP_LOGW(
            TAG,
            "No enabled/configured tracks for background mode=%u",
            static_cast<unsigned>(background_mode)
        );
        return;
    }

    ESP_LOGI(
        TAG,
        "Background mode=%u track=%u index=%u",
        static_cast<unsigned>(background_mode),
        static_cast<unsigned>(track),
        static_cast<unsigned>(playlist_index)
    );

    if (dfplayer_play_track(track) == ESP_OK)
    {
        waiting_for_track_start = true;
        saw_track_playing = false;
        command_time_ms = now_ms();
        idle_since_ms = 0;
    }
}

void begin_background_after_announcement(
    AudioBackgroundMode mode,
    uint16_t announcement_track
)
{
    const AudioPlaylistConfig* requested = nullptr;

    switch (mode)
    {
        case AudioBackgroundMode::POMODORO:
            requested = &audio_config.pomodoro;
            break;

        case AudioBackgroundMode::MEDITATION:
            requested = &audio_config.meditation;
            break;

        case AudioBackgroundMode::HEALING:
            requested = &audio_config.healing;
            break;

        case AudioBackgroundMode::NONE:
        default:
            break;
    }

    if (
        requested == nullptr ||
        !requested->enabled ||
        requested->track_count == 0
    )
    {
        ESP_LOGW(
            TAG,
            "Announcement track=%u played, but background mode=%u has no configured music",
            static_cast<unsigned>(announcement_track),
            static_cast<unsigned>(mode)
        );

        dfplayer_play_track(announcement_track);
        return;
    }

    if (background_mode != AudioBackgroundMode::NONE)
    {
        dfplayer_stop();
    }

    background_mode = mode;
    playlist_index = 0;
    reset_track_detection();
    reset_announcement_detection();

    ESP_LOGI(
        TAG,
        "Playing mode=%u announcement track=%u before background music",
        static_cast<unsigned>(mode),
        static_cast<unsigned>(announcement_track)
    );

    if (dfplayer_play_track(announcement_track) == ESP_OK)
    {
        background_announcement_pending = true;
        pending_announcement_track = announcement_track;
        announcement_command_ms = now_ms();
    }
    else
    {
        ESP_LOGW(TAG, "Announcement command failed; starting background immediately");
        play_current_background_track();
    }
}

void start_background(AudioBackgroundMode mode)
{
    const AudioPlaylistConfig* requested = nullptr;

    switch (mode)
    {
        case AudioBackgroundMode::POMODORO:
            requested = &audio_config.pomodoro;
            break;

        case AudioBackgroundMode::MEDITATION:
            requested = &audio_config.meditation;
            break;

        case AudioBackgroundMode::HEALING:
            requested = &audio_config.healing;
            break;

        case AudioBackgroundMode::NONE:
        default:
            break;
    }

    if (
        requested == nullptr ||
        !requested->enabled ||
        requested->track_count == 0
    )
    {
        ESP_LOGW(
            TAG,
            "Background mode=%u is disabled or has no tracks",
            static_cast<unsigned>(mode)
        );
        return;
    }

    if (background_mode == mode)
    {
        return;
    }

    /*
     * Only one background owner is allowed. Starting a new mode
     * replaces the previous one.
     */
    if (background_mode != AudioBackgroundMode::NONE)
    {
        dfplayer_stop();
    }

    background_mode = mode;
    playlist_index = 0;
    reset_track_detection();
    play_current_background_track();
}


bool minute_is_inside_window(
    int current_minute,
    int start_minute,
    int end_minute
)
{
    if (start_minute == end_minute)
    {
        /*
         * Equal start/end is treated as a full-day window.
         */
        return true;
    }

    if (start_minute < end_minute)
    {
        return
            current_minute >= start_minute &&
            current_minute < end_minute;
    }

    /*
     * Overnight window, for example 22:00 to 06:00.
     */
    return
        current_minute >= start_minute ||
        current_minute < end_minute;
}

int active_healing_schedule_index(time_t now)
{
    if (
        !audio_config.healing.enabled ||
        audio_config.healing.track_count == 0 ||
        audio_config.healing_schedule_count == 0
    )
    {
        return -1;
    }

    struct tm local_time = {};

    if (localtime_r(&now, &local_time) == nullptr)
    {
        return -1;
    }

    const int current_minute =
        local_time.tm_hour * 60 +
        local_time.tm_min;

    for (
        std::size_t index = 0;
        index < audio_config.healing_schedule_count;
        ++index
    )
    {
        const HealingScheduleConfig& schedule =
            audio_config.healing_schedules[index];

        if (!schedule.enabled)
        {
            continue;
        }

        const int start_minute =
            schedule.start_hour * 60 +
            schedule.start_minute;

        const int end_minute =
            schedule.end_hour * 60 +
            schedule.end_minute;

        if (
            minute_is_inside_window(
                current_minute,
                start_minute,
                end_minute
            )
        )
        {
            return static_cast<int>(index);
        }
    }

    return -1;
}

void update_healing_schedule(time_t now)
{
    const int schedule_index =
        active_healing_schedule_index(now);

    const bool should_play =
        schedule_index >= 0;

    if (should_play)
    {
        /*
         * Pomodoro has priority while it is active. Healing will
         * automatically resume after Pomodoro exits, provided the
         * current time remains inside the configured window.
         */
        if (background_mode == AudioBackgroundMode::NONE)
        {
            ESP_LOGI(
                TAG,
                "Healing schedule %d is active",
                schedule_index
            );

            audio_manager_start_healing();
        }

        return;
    }

    if (background_mode == AudioBackgroundMode::HEALING)
    {
        stop_background(
            AudioBackgroundMode::HEALING
        );
    }
}

void stop_background(AudioBackgroundMode mode)
{
    if (background_mode != mode)
    {
        return;
    }

    background_mode = AudioBackgroundMode::NONE;
    playlist_index = 0;
    reset_track_detection();
    reset_announcement_detection();

    dfplayer_stop();
    ESP_LOGI(TAG, "Background playback stopped");
}
}

esp_err_t audio_manager_init()
{
    background_mode = AudioBackgroundMode::NONE;
    mode_before_meditation = AudioBackgroundMode::NONE;
    playlist_index = 0;
    reset_track_detection();
    reset_announcement_detection();

    return dfplayer_init(audio_config.volume);
}

void audio_manager_set_config(
    const AudioManagerConfig& config
)
{
    const AudioBackgroundMode previous_mode =
        background_mode;

    const bool was_running =
        previous_mode != AudioBackgroundMode::NONE;

    if (was_running)
    {
        dfplayer_stop();
    }

    audio_config = config;
    audio_config.volume =
        std::min<uint8_t>(audio_config.volume, 30);

    dfplayer_set_volume(audio_config.volume);

    background_mode = AudioBackgroundMode::NONE;
    playlist_index = 0;
    reset_track_detection();
    reset_announcement_detection();

    ESP_LOGI(
        TAG,
        "Audio JSON applied: volume=%u, pomodoro_tracks=%u, meditation_tracks=%u, healing_tracks=%u, healing_schedules=%u",
        static_cast<unsigned>(audio_config.volume),
        static_cast<unsigned>(audio_config.pomodoro.track_count),
        static_cast<unsigned>(audio_config.meditation.track_count),
        static_cast<unsigned>(audio_config.healing.track_count),
        static_cast<unsigned>(audio_config.healing_schedule_count)
    );

    /*
     * If JSON changes while a session is active, restart that same
     * session using the newly configured playlist.
     */
    if (was_running)
    {
        start_background(previous_mode);
    }
}

const AudioManagerConfig& audio_manager_get_config()
{
    return audio_config;
}

void audio_manager_update(time_t now)
{
    update_healing_schedule(now);
    dfplayer_update();

    /*
     * BUSY is LOW while DFPlayer is playing.
     * Wait for announcement start (LOW), then completion (HIGH),
     * before starting the configured background playlist.
     */
    if (background_announcement_pending)
    {
        const int64_t current_ms = now_ms();
        const bool playing = dfplayer_is_playing();

        if (playing)
        {
            announcement_saw_busy_low = true;
            return;
        }

        if (
            announcement_saw_busy_low &&
            current_ms - announcement_command_ms >= 200
        )
        {
            ESP_LOGI(
                TAG,
                "Announcement track=%u finished; starting background music",
                static_cast<unsigned>(pending_announcement_track)
            );

            reset_announcement_detection();
            play_current_background_track();
            return;
        }

        /*
         * If BUSY wiring is wrong or the track cannot start, avoid
         * blocking the session forever.
         */
        if (current_ms - announcement_command_ms >= 10000)
        {
            ESP_LOGW(
                TAG,
                "Announcement BUSY timeout; starting background music"
            );

            reset_announcement_detection();
            play_current_background_track();
        }

        return;
    }

    const AudioPlaylistConfig* playlist =
        active_playlist();

    if (
        background_mode == AudioBackgroundMode::NONE ||
        playlist == nullptr ||
        !playlist->enabled ||
        playlist->track_count == 0
    )
    {
        return;
    }

    const int64_t current_ms = now_ms();
    const bool playing = dfplayer_is_playing();

    if (playing)
    {
        saw_track_playing = true;
        waiting_for_track_start = false;
        idle_since_ms = 0;
        return;
    }

    if (
        waiting_for_track_start &&
        current_ms - command_time_ms < 1500
    )
    {
        return;
    }

    if (!saw_track_playing)
    {
        if (current_ms - command_time_ms >= 1500)
        {
            ESP_LOGW(TAG, "Track did not start; retrying");
            play_current_background_track();
        }

        return;
    }

    if (idle_since_ms == 0)
    {
        idle_since_ms = current_ms;
        return;
    }

    if (current_ms - idle_since_ms < 500)
    {
        return;
    }

    playlist_index =
        (playlist_index + 1) %
        playlist->track_count;

    play_current_background_track();
}

void audio_manager_start_pomodoro()
{
    audio_manager_start_pomodoro_focus();
}

void audio_manager_start_pomodoro_focus()
{
    begin_background_after_announcement(
        AudioBackgroundMode::POMODORO,
        TONE_POMO_FOCUS
    );
}

void audio_manager_start_pomodoro_break()
{
    begin_background_after_announcement(
        AudioBackgroundMode::POMODORO,
        TONE_POMO_BREAK
    );
}

void audio_manager_stop_pomodoro()
{
    stop_background(AudioBackgroundMode::POMODORO);
}


void audio_manager_start_meditation()
{
    ESP_LOGI(TAG, "Starting meditation background music");

    if (background_mode == AudioBackgroundMode::MEDITATION)
    {
        return;
    }

    mode_before_meditation = background_mode;
    start_background(AudioBackgroundMode::MEDITATION);
}

void audio_manager_stop_meditation()
{
    if (background_mode != AudioBackgroundMode::MEDITATION)
    {
        mode_before_meditation = AudioBackgroundMode::NONE;
        return;
    }

    const AudioBackgroundMode restore_mode =
        mode_before_meditation;

    mode_before_meditation = AudioBackgroundMode::NONE;
    stop_background(AudioBackgroundMode::MEDITATION);

    if (restore_mode != AudioBackgroundMode::NONE)
    {
        ESP_LOGI(
            TAG,
            "Restoring background mode=%u after meditation",
            static_cast<unsigned>(restore_mode)
        );

        start_background(restore_mode);
    }
}

void audio_manager_start_healing()
{
    begin_background_after_announcement(
        AudioBackgroundMode::HEALING,
        TONE_HEALING
    );
}

void audio_manager_stop_healing()
{
    stop_background(AudioBackgroundMode::HEALING);
}

void audio_manager_play_reminder(ReminderType type)
{
    const uint16_t track =
        reminder_track(type);

    if (background_mode != AudioBackgroundMode::NONE)
    {
        ESP_LOGI(
            TAG,
            "Advertisement announcement track=%u",
            static_cast<unsigned>(track)
        );

        dfplayer_play_advertisement(track);
        return;
    }

    ESP_LOGI(
        TAG,
        "Standalone announcement track=%u",
        static_cast<unsigned>(track)
    );

    dfplayer_play_track(track);
}

void audio_manager_play_welcome()
{
    ESP_LOGI(
        TAG,
        "Playing welcome announcement track=%u",
        static_cast<unsigned>(TONE_WELCOME)
    );

    dfplayer_play_track(TONE_WELCOME);
}

AudioBackgroundMode audio_manager_get_background_mode()
{
    return background_mode;
}