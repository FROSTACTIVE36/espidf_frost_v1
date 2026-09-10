#pragma once

#include <cstddef>
#include <cstdint>

enum class ActionLogSource : uint8_t
{
    NONE = 0,
    BOTTLE,
    BLUETOOTH,
    VOLUME,
    CONFIGURATION,
    OTA
};

struct ActionLogSnapshot
{
    bool visible = false;
    ActionLogSource source = ActionLogSource::NONE;
    bool ota_active = false;
    bool indeterminate = false;
    int progress_percentage = -1;
    uint32_t animation_phase = 0;
    uint32_t visible_elapsed_ms = 0;
    uint32_t visible_remaining_ms = 0;
    char message[64] = {};
};

void action_log_init();
void action_log_update();

void action_log_show_bottle(
    const char* message,
    uint32_t duration_ms = 2500
);

void action_log_show_bluetooth(
    bool connected,
    uint32_t duration_ms = 2500
);

void action_log_show_volume(
    uint8_t volume,
    uint32_t duration_ms = 3000
);

void action_log_show_configuration_updated(uint32_t duration_ms = 3000);

void action_log_show_ota(
    const char* message,
    bool indeterminate = true
);

void action_log_show_ota_progress(int percentage);
void action_log_show_ota_result(const char* message, uint32_t duration_ms);
void action_log_clear_ota();

bool action_log_is_visible();
bool action_log_get_snapshot(ActionLogSnapshot& snapshot);