#include "action_log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

namespace
{
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

bool initialized = false;
bool visible = false;
ActionLogSource source = ActionLogSource::NONE;
bool ota_active = false;
bool indeterminate = false;
int progress_percentage = -1;
uint64_t expires_at_ms = 0;
uint64_t visible_since_ms = 0;
char message[64] = {};

uint64_t now_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

void copy_message(const char* text)
{
    if (text == nullptr)
    {
        message[0] = '\0';
        return;
    }

    std::strncpy(message, text, sizeof(message) - 1);
    message[sizeof(message) - 1] = '\0';
}
}

void action_log_init()
{
    taskENTER_CRITICAL(&lock);
    initialized = true;
    visible = false;
    source = ActionLogSource::NONE;
    ota_active = false;
    indeterminate = false;
    progress_percentage = -1;
    expires_at_ms = 0;
    visible_since_ms = 0;
    message[0] = '\0';
    taskEXIT_CRITICAL(&lock);
}

void action_log_update()
{
    if (!initialized)
    {
        return;
    }

    const uint64_t current = now_ms();

    taskENTER_CRITICAL(&lock);
    if (visible && !ota_active && expires_at_ms != 0 && current >= expires_at_ms)
    {
        visible = false;
        source = ActionLogSource::NONE;
        indeterminate = false;
        progress_percentage = -1;
        expires_at_ms = 0;
        visible_since_ms = 0;
        message[0] = '\0';
    }
    taskEXIT_CRITICAL(&lock);
}

void action_log_show_bottle(const char* text, uint32_t duration_ms)
{
    if (!initialized)
    {
        action_log_init();
    }

    taskENTER_CRITICAL(&lock);

    // OTA always has display priority over bottle events.
    if (!ota_active)
    {
        visible = true;
        source = ActionLogSource::BOTTLE;
        indeterminate = false;
        progress_percentage = -1;
        const uint64_t current = now_ms();
        expires_at_ms = current + duration_ms;
        visible_since_ms = current;
        copy_message(text);
    }

    taskEXIT_CRITICAL(&lock);
}

void action_log_show_bluetooth(bool connected, uint32_t duration_ms)
{
    if (!initialized)
    {
        action_log_init();
    }

    taskENTER_CRITICAL(&lock);

    // OTA keeps priority over temporary Bluetooth messages.
    if (!ota_active)
    {
        visible = true;
        source = ActionLogSource::BLUETOOTH;
        indeterminate = false;
        progress_percentage = -1;

        const uint64_t current = now_ms();
        expires_at_ms = current + duration_ms;
        visible_since_ms = current;

        copy_message(
            connected
                ? "connected"
                : "disconnected"
        );
    }

    taskEXIT_CRITICAL(&lock);
}

void action_log_show_volume(uint8_t volume, uint32_t duration_ms)
{
    if (!initialized)
    {
        action_log_init();
    }

    taskENTER_CRITICAL(&lock);

    // OTA always keeps display priority over temporary volume notifications.
    if (!ota_active)
    {
        visible = true;

        /*
         * Reuse the existing Bluetooth Action Log presentation so no display
         * changes are required. The message identifies this as a volume event.
         */
        source = ActionLogSource::VOLUME;
        indeterminate = false;
        progress_percentage = -1;

        const uint64_t current = now_ms();
        expires_at_ms = current + duration_ms;
        visible_since_ms = current;

        char volume_message[64] = {};

        if (volume == 0)
        {
            std::snprintf(
                volume_message,
                sizeof(volume_message),
                "Muted"
            );
        }
        else
        {
            std::snprintf(
                volume_message,
                sizeof(volume_message),
                "Volume %u",
                static_cast<unsigned>(volume)
            );
        }

        copy_message(volume_message);
    }

    taskEXIT_CRITICAL(&lock);
}

void action_log_show_configuration_updated(uint32_t duration_ms)
{
    if (!initialized)
    {
        action_log_init();
    }

    taskENTER_CRITICAL(&lock);

    // OTA keeps priority over temporary configuration notifications.
    if (!ota_active)
    {
        visible = true;
        source = ActionLogSource::CONFIGURATION;
        indeterminate = false;
        progress_percentage = -1;

        const uint64_t current = now_ms();
        expires_at_ms = current + duration_ms;
        visible_since_ms = current;

        copy_message("Setup Revised");
    }

    taskEXIT_CRITICAL(&lock);
}

void action_log_show_ota(const char* text, bool show_indeterminate)
{
    if (!initialized)
    {
        action_log_init();
    }

    taskENTER_CRITICAL(&lock);

    const bool starting_new_ota =
        !visible ||
        source != ActionLogSource::OTA;

    visible = true;
    source = ActionLogSource::OTA;
    ota_active = true;
    indeterminate = show_indeterminate;
    progress_percentage = -1;
    expires_at_ms = 0;

    /*
     * Preserve the existing Dynamic Island animation while OTA changes
     * state, for example:
     *
     * WiFi connecting -> Checking update -> Downloading.
     *
     * Reset the entry animation only when OTA first becomes visible.
     */
    if (starting_new_ota)
    {
        visible_since_ms = now_ms();
    }

    copy_message(text);

    taskEXIT_CRITICAL(&lock);
}

void action_log_show_ota_progress(int percentage)
{
    const int bounded = std::clamp(percentage, 0, 100);

    taskENTER_CRITICAL(&lock);
    const bool starting_new_ota = !visible || source != ActionLogSource::OTA;
    visible = true;
    source = ActionLogSource::OTA;
    ota_active = true;
    indeterminate = false;
    if (starting_new_ota)
    {
        visible_since_ms = now_ms();
    }
    progress_percentage = bounded;
    expires_at_ms = 0;

    char progress_message[64] = {};
    std::snprintf(progress_message, sizeof(progress_message), "Downloading %d%%", bounded);
    copy_message(progress_message);
    taskEXIT_CRITICAL(&lock);
}

void action_log_show_ota_result(const char* text, uint32_t duration_ms)
{
    taskENTER_CRITICAL(&lock);
    visible = true;
    source = ActionLogSource::OTA;
    ota_active = false;
    indeterminate = false;
    progress_percentage = 100;
    const uint64_t current = now_ms();
    expires_at_ms = current + duration_ms;
    visible_since_ms = current;
    copy_message(text);
    taskEXIT_CRITICAL(&lock);
}

void action_log_clear_ota()
{
    taskENTER_CRITICAL(&lock);
    if (source == ActionLogSource::OTA)
    {
        visible = false;
        source = ActionLogSource::NONE;
        ota_active = false;
        indeterminate = false;
        progress_percentage = -1;
        expires_at_ms = 0;
        visible_since_ms = 0;
        message[0] = '\0';
    }
    taskEXIT_CRITICAL(&lock);
}

bool action_log_is_visible()
{
    action_log_update();

    taskENTER_CRITICAL(&lock);
    const bool result = visible;
    taskEXIT_CRITICAL(&lock);
    return result;
}

bool action_log_get_snapshot(ActionLogSnapshot& snapshot)
{
    action_log_update();

    taskENTER_CRITICAL(&lock);
    snapshot.visible = visible;
    snapshot.source = source;
    snapshot.ota_active = ota_active;
    snapshot.indeterminate = indeterminate;
    snapshot.progress_percentage = progress_percentage;
    const uint64_t current = now_ms();
    snapshot.animation_phase = static_cast<uint32_t>((current / 35ULL) % 360ULL);
    snapshot.visible_elapsed_ms =
        (visible && visible_since_ms != 0 && current >= visible_since_ms)
            ? static_cast<uint32_t>(std::min<uint64_t>(current - visible_since_ms, UINT32_MAX))
            : 0;
    snapshot.visible_remaining_ms =
        (visible && !ota_active && expires_at_ms > current)
            ? static_cast<uint32_t>(std::min<uint64_t>(expires_at_ms - current, UINT32_MAX))
            : 0;
    std::strncpy(snapshot.message, message, sizeof(snapshot.message) - 1);
    snapshot.message[sizeof(snapshot.message) - 1] = '\0';
    taskEXIT_CRITICAL(&lock);

    return snapshot.visible;
}