#include "demo_mode.hpp"

#include <cstddef>
#include <cstdint>

#include "esp_log.h"
#include "esp_timer.h"

#include "audio_manager.hpp"
#include "display.hpp"
#include "reminder_types.hpp"

namespace
{
static const char* TAG = "FROST_DEMO";

/*
 * Each frame remains visible for this duration before the next reminder
 * screen/audio pair is shown.
 *
 * Change only this value if you want a faster/slower showroom demo.
 */
static constexpr uint32_t DEMO_FRAME_MS = 5000;

/*
 * Demo sequence:
 * hydration -> stretch -> eye -> walk -> bottle clean ->
 * medication -> custom -> meditation -> repeat.
 *
 * Medication/custom use fixed demo labels only; normal JSON reminders are
 * untouched.
 */
enum class DemoFrame : uint8_t
{
    HYDRATION = 0,
    STRETCH,
    EYE,
    WALK,
    BOTTLE_CLEAN,
    MEDICATION,
    CUSTOM,
    MEDITATION,
    COUNT
};

bool active = false;
DemoFrame current_frame = DemoFrame::HYDRATION;
uint64_t frame_started_ms = 0;

uint64_t now_ms()
{
    return static_cast<uint64_t>(
        esp_timer_get_time() / 1000ULL
    );
}

void show_frame(DemoFrame frame)
{
    switch (frame)
    {
        case DemoFrame::HYDRATION:
            ESP_LOGI(TAG, "Demo: hydration");
            display_show_hydration_reminder();
            audio_manager_play_reminder(ReminderType::HYDRATION);
            break;

        case DemoFrame::STRETCH:
            ESP_LOGI(TAG, "Demo: stretch");
            display_show_stretch_reminder();
            audio_manager_play_reminder(ReminderType::STRETCH);
            break;

        case DemoFrame::EYE:
            ESP_LOGI(TAG, "Demo: eye break");
            display_show_eye_reminder();
            audio_manager_play_reminder(ReminderType::EYE);
            break;

        case DemoFrame::WALK:
            ESP_LOGI(TAG, "Demo: walk");
            display_show_walk_reminder();
            audio_manager_play_reminder(ReminderType::WALK);
            break;

        case DemoFrame::BOTTLE_CLEAN:
            ESP_LOGI(TAG, "Demo: bottle clean");
            display_show_bottle_clean_reminder();
            audio_manager_play_reminder(ReminderType::BOTTLE_CLEAN);
            break;

        case DemoFrame::MEDICATION:
            ESP_LOGI(TAG, "Demo: medication");
            display_show_medication_reminder(
                "Vitamin D",
                120,
                160,
                1,
                65535,
                1,
                180
            );
            audio_manager_play_reminder(ReminderType::MEDICATION);
            break;

        case DemoFrame::CUSTOM:
            ESP_LOGI(TAG, "Demo: custom reminder");
            display_show_custom_reminder(
                "Water plants",
                120,
                165,
                1,
                65535,
                1,
                180
            );
            audio_manager_play_reminder(ReminderType::CUSTOM);
            break;

        case DemoFrame::MEDITATION:
            ESP_LOGI(TAG, "Demo: meditation");
            display_show_meditation_reminder();

            /*
             * For demo mode we play the meditation announcement/track once.
             * We deliberately do not start the normal meditation background
             * owner because demo advances to another screen after 5 seconds.
             */
            audio_manager_play_reminder(ReminderType::MEDITATION);
            break;

        case DemoFrame::COUNT:
        default:
            break;
    }
}

DemoFrame next_frame(DemoFrame frame)
{
    const uint8_t next =
        (static_cast<uint8_t>(frame) + 1U) %
        static_cast<uint8_t>(DemoFrame::COUNT);

    return static_cast<DemoFrame>(next);
}
}

void demo_mode_init()
{
    active = false;
    current_frame = DemoFrame::HYDRATION;
    frame_started_ms = 0;
}

void demo_mode_start()
{
    if (active)
    {
        return;
    }

    active = true;
    current_frame = DemoFrame::HYDRATION;
    frame_started_ms = now_ms();

    ESP_LOGI(TAG, "Demo mode started");
    show_frame(current_frame);
}

void demo_mode_stop()
{
    if (!active)
    {
        return;
    }

    active = false;
    frame_started_ms = 0;

    ESP_LOGI(TAG, "Demo mode stopped");
}

void demo_mode_update()
{
    if (!active)
    {
        return;
    }

    const uint64_t current_ms = now_ms();

    if (current_ms - frame_started_ms < DEMO_FRAME_MS)
    {
        return;
    }

    current_frame = next_frame(current_frame);
    frame_started_ms = current_ms;

    show_frame(current_frame);
}

bool demo_mode_is_active()
{
    return active;
}