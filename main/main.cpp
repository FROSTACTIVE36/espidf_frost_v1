#include <cstdio>
#include <ctime>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "display.hpp"
#include "reminder_engine.hpp"
#include "reminder_types.hpp"
#include "config_parser.hpp"

/* =========================================================
 * Logging
 * ========================================================= */

static const char* TAG = "FROST_MAIN";

/* =========================================================
 * Reminder JSON
 * =========================================================
 *
 * You can later replace this embedded JSON with:
 *
 * - SPIFFS JSON
 * - NVS configuration
 * - Bluetooth configuration
 * - Wi-Fi configuration
 * - Mobile application configuration
 *
 * For initial testing, the JSON is stored directly here.
 */

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
      "snooze_min": 5,
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
      "snooze_min": 5,
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
      "snooze_min": 5,
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
      "snooze_min": 5,
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
      "snooze_min": 5,
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
      "snooze_min": 5,

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
  }
}
)json";

/* =========================================================
 * Reminder trigger callback
 * =========================================================
 *
 * This function is called by reminder_engine.cpp whenever a
 * reminder becomes active.
 */

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

    switch (type)
    {
        /* -------------------------------------------------
         * Hydration
         * ------------------------------------------------- */

        case ReminderType::HYDRATION:
        {
            ESP_LOGI(
                TAG,
                "Showing hydration reminder"
            );

            display_show_hydration_reminder();

            break;
        }

        /* -------------------------------------------------
         * Stretch
         * ------------------------------------------------- */

        case ReminderType::STRETCH:
        {
            ESP_LOGI(
                TAG,
                "Showing stretch reminder"
            );

            display_show_stretch_reminder();

            break;
        }

        /* -------------------------------------------------
         * Eye break
         * ------------------------------------------------- */

        case ReminderType::EYE:
        {
            ESP_LOGI(
                TAG,
                "Showing eye reminder"
            );

            display_show_eye_reminder();

            break;
        }

        /* -------------------------------------------------
         * Walk
         * ------------------------------------------------- */

        case ReminderType::WALK:
        {
            ESP_LOGI(
                TAG,
                "Showing walk reminder"
            );

            display_show_walk_reminder();

            break;
        }

        /* -------------------------------------------------
         * Meditation
         *
         * Meditation uses a complete image.
         * No dynamic text is drawn.
         * ------------------------------------------------- */

        case ReminderType::MEDITATION:
        {
            ESP_LOGI(
                TAG,
                "Showing meditation reminder"
            );

            display_show_meditation_reminder();

            break;
        }

        /* -------------------------------------------------
         * Medication
         *
         * item_index identifies the medicine.
         * schedule_index identifies the medicine dose.
         * ------------------------------------------------- */

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

        /* -------------------------------------------------
         * Custom reminder
         *
         * item_index identifies the custom event.
         * ------------------------------------------------- */

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
 * =========================================================
 *
 * Called when:
 *
 * - display timeout completes
 * - reminder is acknowledged
 * - reminder is cancelled
 * - reminder is snoozed
 */

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
}

/* =========================================================
 * Load reminder configuration
 * ========================================================= */

static bool load_reminder_configuration()
{
    char parser_error[160] = {};

    const bool parsed =
        reminder_config_parse_and_apply(
            REMINDER_JSON,
            parser_error,
            sizeof(parser_error)
        );

    if (!parsed)
    {
        ESP_LOGE(
            TAG,
            "Failed to parse reminder JSON: %s",
            parser_error
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "Reminder JSON loaded successfully"
    );

    return true;
}

/* =========================================================
 * Validate system time
 * =========================================================
 *
 * The reminder system requires valid date and time.
 *
 * If you already set the time from DS3231, SNTP, UART or NVS,
 * this function will return true.
 */

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

/* =========================================================
 * Wait for valid time
 * =========================================================
 *
 * This does not set time. It only warns when time is invalid.
 * Your DS3231 or SNTP code should set the system time before
 * reminders are checked.
 */

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
 * Optional reminder action functions
 * =========================================================
 *
 * Call these functions from your physical button handler,
 * touch handler, BLE command or UART command.
 */

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

    /* -----------------------------------------------------
     * Show Frost boot logo
     * ----------------------------------------------------- */

    display_show_frost_logo();

    vTaskDelay(
        pdMS_TO_TICKS(2000)
    );

    /* -----------------------------------------------------
     * Initialize reminder engine
     * ----------------------------------------------------- */

    reminder_engine_init();

    reminder_engine_set_trigger_callback(
        on_reminder_triggered
    );

    reminder_engine_set_finished_callback(
        on_reminder_finished
    );

    /* -----------------------------------------------------
     * Load reminder JSON
     * ----------------------------------------------------- */

    if (!load_reminder_configuration())
    {
        ESP_LOGE(
            TAG,
            "Reminder configuration could not be loaded"
        );
    }

    /* -----------------------------------------------------
     * Log current date and time
     * ----------------------------------------------------- */

    log_system_time();

    ESP_LOGI(
        TAG,
        "Entering main loop"
    );

    /* -----------------------------------------------------
     * Main application loop
     * ----------------------------------------------------- */

    while (true)
    {
        const time_t now =
            time(nullptr);

        /*
         * Check every reminder configuration.
         *
         * The engine will call on_reminder_triggered()
         * when a reminder becomes active.
         */
        reminder_engine_update(
            now
        );

        /*
         * Only show the clock when no reminder is active.
         *
         * This prevents the clock screen from immediately
         * overwriting a reminder screen.
         */
        if (!reminder_engine_has_active_reminder())
        {
            display_show_home_clock(
                now
            );
        }

        /*
         * Check reminders four times per second.
         *
         * Absolute medication/custom reminders are still
         * protected against firing repeatedly during the
         * same minute.
         */
        vTaskDelay(
            pdMS_TO_TICKS(250)
        );
    }
}