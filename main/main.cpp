#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "acknowledgement_input.hpp"
#include "audio_manager.hpp"
#include "bluetooth.hpp"
#include "config_parser.hpp"
#include "display.hpp"
#include "pomodoro.hpp"
#include "reminder_engine.hpp"
#include "reminder_types.hpp"
#include "rtc_ds3231.hpp"
#include "configuration_storage.hpp"

/* =========================================================
 * Logging
 * ========================================================= */

static const char* TAG = "FROST_MAIN";


/* =========================================================
 * Pomodoro double-tap detection
 * ========================================================= */

static bool pomodoro_first_tap_pending = false;
static uint64_t pomodoro_first_tap_ms = 0;

static constexpr uint64_t POMODORO_DOUBLE_TAP_MIN_MS = 120;
static constexpr uint64_t POMODORO_DOUBLE_TAP_MAX_MS = 800;

static uint64_t application_millis()
{
    return static_cast<uint64_t>(
        esp_timer_get_time() / 1000ULL
    );
}

/* =========================================================
 * Reminder and Pomodoro JSON
 * ========================================================= */

static const char* REMINDER_JSON = R"json(
{
  "_meta": {
    "schema_ver": 6,
    "device": "FROST"
  },

  "reminders": {
    "hydration": {
      "enabled": true,
      "mode": "interval",
      "interval_ms": 60000,
      "display_ms": 10000,
      "require_ack": false,
      "start_hour": 0,
      "start_min": 0,
      "end_hour": 23,
      "end_min": 59,
      "days": []
    },

    "stretch": {
      "enabled": true,
      "mode": "interval",
      "interval_ms": 120000,
      "display_ms": 10000,
      "require_ack": false,
      "start_hour": 0,
      "start_min": 0,
      "end_hour": 23,
      "end_min": 59,
      "days": []
    },

    "eye": {
      "enabled": true,
      "mode": "interval",
      "interval_ms": 180000,
      "display_ms": 10000,
      "require_ack": false,
      "start_hour": 0,
      "start_min": 0,
      "end_hour": 23,
      "end_min": 59,
      "days": []
    },

    "walk": {
      "enabled": true,
      "mode": "interval",
      "interval_ms": 240000,
      "display_ms": 10000,
      "require_ack": false,
      "start_hour": 0,
      "start_min": 0,
      "end_hour": 23,
      "end_min": 59,
      "days": []
    },

    "meditation": {
      "enabled": false,
      "sh": 6,
      "sm": 0,
      "eh": 7,
      "em": 0,
      "display_sec": 600,
      "require_ack": false,
      "days": []
    },

    "medication": {
      "enabled": true,
      "require_ack": false,
      "snooze_min": 10,
      "display_ms": 15000,

      "medicines": [
        {
          "id": "med_001",
          "label": "Vitamin D",
          "enabled": true,

          "start": "2026-01-01",
          "end": "2026-12-31",

          "days": null,

          "text_x": 120,
          "text_y": 160,
          "text_size": 2,
          "text_color": 65535,
          "text_align": 1,
          "text_width": 180,

          "doses": [
            {
              "h": 8,
              "m": 0
            },
            {
              "h": 12,
              "m": 0
            },
            {
              "h": 18,
              "m": 0
            }
          ]
        },

        {
          "id": "med_002",
          "label": "BP Tablet",
          "enabled": true,

          "start": "2026-01-01",
          "end": "2026-12-31",

          "days": [],

          "text_x": 120,
          "text_y": 160,
          "text_size": 2,
          "text_color": 65535,
          "text_align": 1,
          "text_width": 180,

          "doses": [
            {
              "h": 9,
              "m": 0
            },
            {
              "h": 21,
              "m": 0
            }
          ]
        }
      ]
    },

    "custom": {
      "enabled": true,
      "require_ack": false,

      "events": [
        {
          "id": "custom_001",
          "label": "Water plants",
          "enabled": true,

          "h": 9,
          "m": 0,

          "show_ms": 15000,
          "type": "recurring",

          "days": [
            "mon",
            "wed"
          ],

          "text_x": 120,
          "text_y": 165,
          "text_size": 2,
          "text_color": 65535,
          "text_align": 1,
          "text_width": 180
        },

        {
          "id": "custom_002",
          "label": "Doctor appointment",
          "enabled": true,

          "h": 14,
          "m": 30,

          "show_ms": 15000,
          "type": "absolute",
          "date": "2026-12-25",

          "text_x": 120,
          "text_y": 160,
          "text_size": 1,
          "text_color": 65535,
          "text_align": 1,
          "text_width": 180
        }
      ]
    }
  },

  "audio": {
    "volume": 20,
    "pomodoro": {
      "enabled": true,
      "tracks": [20, 21]
    },
    "healing": {
      "enabled": true,
      "tracks": [18]
    },
    "healing_schedules": [
      {
        "enabled": true,
        "start_time": "06:00",
        "end_time": "07:00"
      },
      {
        "enabled": true,
        "start_time": "12:30",
        "end_time": "13:00"
      },
      {
        "enabled": true,
        "start_time": "21:00",
        "end_time": "21:30"
      }
    ]
  },

  "pomodoro": {
    "enabled": true,

    "focus_min": 25,
    "break_min": 5,
    "cycles": 4,

    "auto_start_break": true,
    "auto_start_focus": true,

    "lap_mode_enabled": false,
    "laps": [
      {
        "enabled": true,
        "sh": 9,
        "sm": 0,
        "eh": 12,
        "em": 0
      },
      {
        "enabled": true,
        "sh": 14,
        "sm": 0,
        "eh": 17,
        "em": 0
      }
    ],

    "focus_counter": {
      "x": 120,
      "y": 150,
      "text_size": 3,
      "text_color": 65535,
      "text_align": 1
    },

    "break_counter": {
      "x": 120,
      "y": 150,
      "text_size": 3,
      "text_color": 0,
      "text_align": 1
    }
  }
}
)json";

/* =========================================================
 * Reminder trigger callback
 * ========================================================= */

static void on_reminder_triggered(
    ReminderType type,
    int item_index,
    int schedule_index
)
{
    ESP_LOGI(
        TAG,
        "Reminder triggered: type=%d item=%d schedule=%d",
        static_cast<int>(type),
        item_index,
        schedule_index
    );

    /*
     * Meditation uses track 22 as looping background music for the
     * complete meditation reminder duration. Other reminder types
     * use their normal announcement tone.
     */
    if (type == ReminderType::MEDITATION)
    {
        audio_manager_start_meditation();
    }
    else
    {
        audio_manager_play_reminder(type);
    }

    switch (type)
    {
        case ReminderType::HYDRATION:
        {
            ESP_LOGI(
                TAG,
                "Showing hydration reminder"
            );

            display_show_hydration_reminder();

            break;
        }

        case ReminderType::STRETCH:
        {
            ESP_LOGI(
                TAG,
                "Showing stretch reminder"
            );

            display_show_stretch_reminder();

            break;
        }

        case ReminderType::EYE:
        {
            ESP_LOGI(
                TAG,
                "Showing eye reminder"
            );

            display_show_eye_reminder();

            break;
        }

        case ReminderType::WALK:
        {
            ESP_LOGI(
                TAG,
                "Showing walk reminder"
            );

            display_show_walk_reminder();

            break;
        }

        case ReminderType::MEDITATION:
        {
            ESP_LOGI(
                TAG,
                "Showing meditation reminder"
            );

            display_show_meditation_reminder();

            break;
        }

        case ReminderType::MEDICATION:
        {
            const MedicationConfig* medication_config =
                reminder_engine_get_medication_config();

            if (medication_config == nullptr)
            {
                ESP_LOGE(
                    TAG,
                    "Medication configuration is null"
                );

                break;
            }

            if (
                item_index < 0 ||
                item_index >=
                    static_cast<int>(
                        medication_config->medicine_count
                    )
            )
            {
                ESP_LOGE(
                    TAG,
                    "Invalid medication item index: %d",
                    item_index
                );

                break;
            }

            const MedicationItem& medicine =
                medication_config->medicines[
                    item_index
                ];

            ESP_LOGI(
                TAG,
                "Medication: %s, dose index: %d",
                medicine.label,
                schedule_index
            );

            display_show_medication_reminder(
                medicine.label,
                medicine.text_x,
                medicine.text_y,
                medicine.text_size,
                medicine.text_color,
                medicine.text_align,
                medicine.text_width
            );

            break;
        }

        case ReminderType::CUSTOM:
        {
            const CustomReminderConfig* custom_config =
                reminder_engine_get_custom_config();

            if (custom_config == nullptr)
            {
                ESP_LOGE(
                    TAG,
                    "Custom reminder configuration is null"
                );

                break;
            }

            if (
                item_index < 0 ||
                item_index >=
                    static_cast<int>(
                        custom_config->event_count
                    )
            )
            {
                ESP_LOGE(
                    TAG,
                    "Invalid custom event index: %d",
                    item_index
                );

                break;
            }

            const CustomEvent& event =
                custom_config->events[
                    item_index
                ];

            ESP_LOGI(
                TAG,
                "Custom reminder: %s",
                event.label
            );

            display_show_custom_reminder(
                event.label,
                event.text_x,
                event.text_y,
                event.text_size,
                event.text_color,
                event.text_align,
                event.text_width
            );

            break;
        }

        default:
        {
            ESP_LOGW(
                TAG,
                "Unknown reminder type: %d",
                static_cast<int>(type)
            );

            break;
        }
    }
}

/* =========================================================
 * Reminder finished callback
 * ========================================================= */

static void on_reminder_finished(
    ReminderType type,
    int item_index,
    int schedule_index
)
{
    ESP_LOGI(
        TAG,
        "Reminder finished: type=%d item=%d schedule=%d",
        static_cast<int>(type),
        item_index,
        schedule_index
    );

    if (type == ReminderType::MEDITATION)
    {
        audio_manager_stop_meditation();
    }

    /*
     * A reminder may have covered the Pomodoro break screen.
     * Force Pomodoro to redraw after the reminder finishes.
     */
    pomodoro_force_redraw();
}

/* =========================================================
 * Load configuration
 * ========================================================= */

static bool apply_configuration_json(
    const char* json_text,
    const char* source_name
)
{
    if (json_text == nullptr)
    {
        return false;
    }

    char parser_error[160] = {};

    const bool parsed =
        reminder_config_parse_and_apply(
            json_text,
            parser_error,
            sizeof(parser_error)
        );

    if (!parsed)
    {
        ESP_LOGE(
            TAG,
            "Failed to parse %s configuration: %s",
            source_name,
            parser_error[0] != '\0'
                ? parser_error
                : "Unknown parser error"
        );

        return false;
    }

    pomodoro_force_redraw();

    ESP_LOGI(
        TAG,
        "%s configuration applied successfully",
        source_name
    );

    return true;
}

static bool load_startup_configuration()
{
    char* saved_json = nullptr;
    std::size_t saved_length = 0;

    const esp_err_t load_error =
        configuration_storage_load(
            &saved_json,
            &saved_length
        );

    if (load_error == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Applying saved user configuration"
        );

        const bool applied =
            apply_configuration_json(
                saved_json,
                "saved user"
            );

        free(saved_json);

        if (applied)
        {
            return true;
        }

        /*
         * Saved JSON exists but is corrupted or incompatible.
         * Clear it so the same failure does not repeat every boot.
         */
        ESP_LOGW(
            TAG,
            "Saved configuration is invalid; clearing it"
        );

        configuration_storage_clear();
    }
    else if (
        load_error != ESP_ERR_NVS_NOT_FOUND
    )
    {
        ESP_LOGW(
            TAG,
            "Could not load saved configuration: %s",
            esp_err_to_name(load_error)
        );
    }

    ESP_LOGI(
        TAG,
        "No valid saved configuration; applying default JSON"
    );

    return apply_configuration_json(
        REMINDER_JSON,
        "default"
    );
}

/* =========================================================
 * Validate system time
 * ========================================================= */

static bool system_time_is_valid()
{
    const time_t now =
        time(nullptr);

    struct tm time_info = {};

    localtime_r(
        &now,
        &time_info
    );

    const int current_year =
        time_info.tm_year + 1900;

    return current_year >= 2025;
}

static void log_system_time()
{
    const time_t now =
        time(nullptr);

    struct tm time_info = {};

    localtime_r(
        &now,
        &time_info
    );

    ESP_LOGI(
        TAG,
        "Current system time: %04d-%02d-%02d %02d:%02d:%02d",
        time_info.tm_year + 1900,
        time_info.tm_mon + 1,
        time_info.tm_mday,
        time_info.tm_hour,
        time_info.tm_min,
        time_info.tm_sec
    );

    if (!system_time_is_valid())
    {
        ESP_LOGW(
            TAG,
            "System time is invalid. Medication, custom and "
            "absolute reminders may not trigger correctly."
        );
    }
}

/* =========================================================
 * Reminder controls
 * ========================================================= */

static void acknowledge_current_reminder()
{
    if (!reminder_engine_has_active_reminder())
    {
        ESP_LOGI(
            TAG,
            "No active reminder to acknowledge"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Acknowledging active reminder"
    );

    reminder_engine_acknowledge_active();
}

/*
 * Input behaviour:
 *
 * Active reminder:
 *     acknowledge reminder
 *
 * No active reminder:
 *     start or stop Pomodoro
 */
static void on_acknowledgement_input()
{
    /*
     * Active reminder:
     * one tap acknowledges immediately.
     */
    if (reminder_engine_has_active_reminder())
    {
        pomodoro_first_tap_pending = false;
        pomodoro_first_tap_ms = 0;

        acknowledge_current_reminder();
        return;
    }

    const uint64_t now_ms =
        application_millis();

    /*
     * First tap: wait for a second tap.
     */
    if (!pomodoro_first_tap_pending)
    {
        pomodoro_first_tap_pending = true;
        pomodoro_first_tap_ms = now_ms;

        ESP_LOGI(
            TAG,
            "Pomodoro first tap detected"
        );

        return;
    }

    const uint64_t elapsed_ms =
        now_ms - pomodoro_first_tap_ms;

    /*
     * Reject sensor bounce/noise.
     */
    if (elapsed_ms < POMODORO_DOUBLE_TAP_MIN_MS)
    {
        ESP_LOGW(
            TAG,
            "Second tap ignored as bounce: %llu ms",
            static_cast<unsigned long long>(elapsed_ms)
        );

        return;
    }

    /*
     * Valid double tap: toggle once.
     */
    if (elapsed_ms <= POMODORO_DOUBLE_TAP_MAX_MS)
    {
        pomodoro_first_tap_pending = false;
        pomodoro_first_tap_ms = 0;

        ESP_LOGI(
            TAG,
            "Pomodoro double tap detected: %llu ms",
            static_cast<unsigned long long>(elapsed_ms)
        );

        pomodoro_toggle();
        return;
    }

    /*
     * Old tap expired; this becomes the new first tap.
     */
    pomodoro_first_tap_pending = true;
    pomodoro_first_tap_ms = now_ms;
}

static void update_pomodoro_double_tap()
{
    if (!pomodoro_first_tap_pending)
    {
        return;
    }

    const uint64_t now_ms =
        application_millis();

    if (
        now_ms - pomodoro_first_tap_ms >
        POMODORO_DOUBLE_TAP_MAX_MS
    )
    {
        pomodoro_first_tap_pending = false;
        pomodoro_first_tap_ms = 0;

        ESP_LOGI(
            TAG,
            "Pomodoro single tap expired"
        );
    }
}

static void snooze_current_reminder()
{
    if (!reminder_engine_has_active_reminder())
    {
        ESP_LOGI(
            TAG,
            "No active reminder to snooze"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Snoozing active reminder"
    );

    reminder_engine_snooze_active();
}

static void dismiss_current_reminder()
{
    if (!reminder_engine_has_active_reminder())
    {
        ESP_LOGI(
            TAG,
            "No active reminder to dismiss"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Dismissing active reminder"
    );

    reminder_engine_cancel_active();
}

/* =========================================================
 * Apply Bluetooth JSON
 * ========================================================= */

static bool apply_bluetooth_json(
    const char* json_text,
    std::size_t json_length
)
{
    if (
        json_text == nullptr ||
        json_length == 0
    )
    {
        ESP_LOGE(
            TAG,
            "Bluetooth JSON is empty"
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "Applying Bluetooth JSON, size=%u bytes",
        static_cast<unsigned int>(
            json_length
        )
    );

    char parser_error[160] = {};

    const bool applied =
        reminder_config_parse_and_apply(
            json_text,
            parser_error,
            sizeof(parser_error)
        );

    if (!applied)
    {
        ESP_LOGE(
            TAG,
            "Bluetooth JSON rejected: %s",
            parser_error[0] != '\0'
                ? parser_error
                : "Unknown parser error"
        );

        return false;
    }

     const esp_err_t save_error =
        configuration_storage_save(
            json_text,
            json_length
        );

    if (save_error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Configuration applied but could not be saved: %s",
            esp_err_to_name(save_error)
        );

        /*
         * Return false so Bluetooth can report that persistence failed.
         * The configuration is active now, but would be lost on reboot.
         */
        return false;
    }

    /*
     * JSON changes may include counter coordinates or colors.
     */
    pomodoro_force_redraw();

    ESP_LOGI(
        TAG,
        "Bluetooth configuration applied"
    );

    return true;
}

/* =========================================================
 * Initialize NVS
 * ========================================================= */

static bool initialize_nvs()
{
    esp_err_t error =
        nvs_flash_init();

    if (
        error == ESP_ERR_NVS_NO_FREE_PAGES ||
        error == ESP_ERR_NVS_NEW_VERSION_FOUND
    )
    {
        ESP_LOGW(
            TAG,
            "NVS requires erase and reinitialization"
        );

        error =
            nvs_flash_erase();

        if (error != ESP_OK)
        {
            ESP_LOGE(
                TAG,
                "NVS erase failed: %s",
                esp_err_to_name(error)
            );

            return false;
        }

        error =
            nvs_flash_init();
    }

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "NVS initialization failed: %s",
            esp_err_to_name(error)
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "NVS initialized"
    );

    return true;
}

/* =========================================================
 * Initialize RTC
 * ========================================================= */

static void initialize_rtc()
{
    const esp_err_t init_error =
        rtc_ds3231_init();

    if (init_error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "DS3231 initialization failed: %s",
            esp_err_to_name(init_error)
        );

        ESP_LOGW(
            TAG,
            "BLE can start, but SET time will fail until RTC is available"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "DS3231 initialized"
    );

    const esp_err_t sync_error =
        rtc_ds3231_sync_system_time();

    if (sync_error != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Initial RTC synchronization failed: %s",
            esp_err_to_name(sync_error)
        );

        ESP_LOGW(
            TAG,
            "Send SET YYYY-MM-DD HH:MM:SS through BLE"
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "ESP32 system time synchronized from DS3231"
    );
}

/* =========================================================
 * Initialize Bluetooth
 * ========================================================= */

static void initialize_bluetooth()
{
    const esp_err_t bluetooth_error =
        bluetooth_init(
            apply_bluetooth_json
        );

    if (bluetooth_error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Bluetooth initialization failed: %s",
            esp_err_to_name(
                bluetooth_error
            )
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Bluetooth ready; advertising as ESP32_RTC"
    );
}

/* =========================================================
 * Application entry point
 * ========================================================= */

extern "C" void app_main()
{
    ESP_LOGI(
        TAG,
        "================================"
    );

    ESP_LOGI(
        TAG,
        "Starting FROST firmware"
    );

    ESP_LOGI(
        TAG,
        "================================"
    );

    /* -----------------------------------------------------
     * Initialize NVS
     * ----------------------------------------------------- */

    if (!initialize_nvs())
    {
        return;
    }

    /* -----------------------------------------------------
     * Initialize display
     * ----------------------------------------------------- */

    if (!display_init())
    {
        ESP_LOGE(
            TAG,
            "Display initialization failed"
        );

        return;
    }

    /*
     * Keep the FROST logo visible while the DFPlayer starts.
     * Do not send any playback command before audio_manager_init().
     */
    display_show_frost_logo();

    /* -----------------------------------------------------
     * Initialize DFPlayer/audio manager
     * ----------------------------------------------------- */

    const esp_err_t audio_result =
        audio_manager_init();

    if (audio_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Audio initialization failed: %s",
            esp_err_to_name(audio_result)
        );
    }
    else
    {
        /*
         * Play /mp3/0001.mp3 while the FROST logo is visible.
         */
        vTaskDelay(
            pdMS_TO_TICKS(200)
        );

        audio_manager_play_welcome();
    }

    /*
     * Keep the startup logo visible long enough for the welcome note.
     */
    vTaskDelay(
        pdMS_TO_TICKS(2500)
    );

    /* -----------------------------------------------------
     * Initialize RTC
     * ----------------------------------------------------- */

    initialize_rtc();

    /* -----------------------------------------------------
     * Initialize engines
     * ----------------------------------------------------- */

    reminder_engine_init();
    pomodoro_init();

    reminder_engine_set_trigger_callback(
        on_reminder_triggered
    );

    reminder_engine_set_finished_callback(
        on_reminder_finished
    );

    /* -----------------------------------------------------
     * Initialize acknowledgement/Pomodoro input
     * ----------------------------------------------------- */

    AcknowledgementInputConfig acknowledgement_config;

    acknowledgement_config.gpio =
        GPIO_NUM_7;

    acknowledgement_config.active_low =
        true;

    acknowledgement_config.debounce_ms =
        50;

    const esp_err_t acknowledgement_error =
        acknowledgement_input_init(
            acknowledgement_config,
            on_acknowledgement_input
        );

    if (acknowledgement_error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Acknowledgement input initialization failed: %s",
            esp_err_to_name(
                acknowledgement_error
            )
        );
    }

    /* -----------------------------------------------------
     * Load default JSON
     * ----------------------------------------------------- */

    if (!load_startup_configuration())
    {
        ESP_LOGE(
            TAG,
            "No valid configuration could be loaded"
        );
    }

    /* -----------------------------------------------------
     * Initialize Bluetooth
     * ----------------------------------------------------- */

    initialize_bluetooth();

    /* -----------------------------------------------------
     * Show current time in logs
     * ----------------------------------------------------- */

    log_system_time();

    ESP_LOGI(
        TAG,
        "Entering main loop"
    );

    /* -----------------------------------------------------
     * Main loop
     * ----------------------------------------------------- */

    while (true)
    {
        const time_t now =
            time(nullptr);

        /*
         * Read the acknowledgement/Pomodoro input.
         */
        acknowledgement_input_update();
        update_pomodoro_double_tap();

        /*
         * Advances background playlists and reads DFPlayer status.
         */
        audio_manager_update(now);

        const bool pomodoro_focus_active =
            pomodoro_is_running() &&
            pomodoro_get_state() ==
                PomodoroState::FOCUS;

        /*
         * During Pomodoro focus:
         *
         * - medication reminders may become active immediately
         * - all other reminders are still detected and queued
         * - queued non-medication reminders wait until break
         *
         * During break or normal clock mode:
         *
         * - all reminder types may become active normally
         */
        reminder_engine_set_medication_only_activation(
            pomodoro_focus_active
        );

        /*
         * Always update reminders before Pomodoro transitions.
         *
         * This ordering is important when a break reaches 00:00:
         * a due or queued reminder becomes active first, allowing
         * pomodoro_update() to hold the break until it finishes.
         */
        reminder_engine_update(
            now
        );

        /*
         * Update Pomodoro after reminder processing.
         */
        pomodoro_update();

        /*
         * Display priority:
         *
         * 1. Active reminder
         * 2. Pomodoro focus/break screen
         * 3. Home clock
         */
        if (
            !reminder_engine_has_active_reminder()
        )
        {
            if (pomodoro_is_running())
            {
                pomodoro_render_if_needed();
            }
            else
            {
                display_show_home_clock(
                    now
                );
            }
        }

        vTaskDelay(
            pdMS_TO_TICKS(20)
        );
    }
}