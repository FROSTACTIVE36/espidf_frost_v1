#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

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
#include "scale.hpp"
#include "bottle_calibration.hpp"
#include "consumption_tracker.hpp"
#include "statistics_history.hpp"
#include "user_statistics.hpp"
#include "action_log.hpp"
#include "wifi_manager.hpp"
#include "ota_manager.hpp"

/* =========================================================
 * Logging
 * ========================================================= */

static const char* TAG = "FROST_MAIN";

/* =========================================================
 * Top-level system state machine
 * ========================================================= */

enum class SystemState : uint8_t
{
    IDLE = 0,
    REMINDER,
    POMODORO,
    CALIBRATION,
    OTA,
    CONSUMPTION
};

static SystemState system_state = SystemState::IDLE;

static constexpr uint32_t MAIN_LOOP_PERIOD_MS = 20;
static constexpr uint32_t SYSTEM_DIAGNOSTIC_INTERVAL_MS = 5000;

static uint64_t state_entered_ms = 0;
static uint64_t diagnostic_window_started_us = 0;
static uint64_t diagnostic_busy_us = 0;
static uint32_t diagnostic_loop_count = 0;

/*
 * True per-core CPU usage is derived from FreeRTOS run-time statistics.
 *
 * IDLE0 and IDLE1 are pinned to Core 0 and Core 1 respectively, so:
 *
 *     CPU usage = 100% - idle percentage
 *
 * We use deltas between diagnostic windows rather than percentages since
 * boot. This makes the values represent the current 5-second state.
 *
 * The storage is static so diagnostics do not consume the main task stack.
 */
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
static constexpr UBaseType_t CPU_STATS_MAX_TASKS = 48;
static TaskStatus_t cpu_task_status[CPU_STATS_MAX_TASKS] = {};

static uint64_t previous_total_runtime = 0;
static uint64_t previous_idle0_runtime = 0;
static uint64_t previous_idle1_runtime = 0;
static bool cpu_runtime_baseline_valid = false;
#endif

static const char* system_state_name(SystemState state)
{
    switch (state)
    {
        case SystemState::IDLE:
            return "IDLE";

        case SystemState::REMINDER:
            return "REMINDER";

        case SystemState::POMODORO:
            return "POMODORO";

        case SystemState::CALIBRATION:
            return "CALIBRATION";

        case SystemState::OTA:
            return "OTA";

        case SystemState::CONSUMPTION:
            return "CONSUMPTION";

        default:
            return "UNKNOWN";
    }
}

static void transition_system_state(
    SystemState new_state,
    const char* reason
)
{
    if (new_state == system_state)
    {
        return;
    }

    const uint64_t current_ms =
        static_cast<uint64_t>(
            esp_timer_get_time() / 1000ULL
        );

    const uint64_t previous_duration_ms =
        state_entered_ms == 0
            ? 0
            : current_ms - state_entered_ms;

    ESP_LOGI(
        TAG,
        "STATE: %s -> %s | reason=%s | previous_duration=%llu ms",
        system_state_name(system_state),
        system_state_name(new_state),
        reason != nullptr ? reason : "none",
        static_cast<unsigned long long>(previous_duration_ms)
    );

    system_state = new_state;
    state_entered_ms = current_ms;
}

static SystemState determine_system_state()
{
    /*
     * Priority order:
     *
     * 1. Calibration
     * 2. OTA
     * 3. Active reminder
     * 4. Consumption result screen
     * 5. Pomodoro
     * 6. Idle/home clock
     */
    if (bottle_calibration_is_active())
    {
        return SystemState::CALIBRATION;
    }

    if (ota_manager_is_busy())
    {
        return SystemState::OTA;
    }

    if (reminder_engine_has_active_reminder())
    {
        return SystemState::REMINDER;
    }

    if (consumption_tracker_screen_active())
    {
        return SystemState::CONSUMPTION;
    }

    if (pomodoro_is_running())
    {
        return SystemState::POMODORO;
    }

    return SystemState::IDLE;
}

struct CpuUsageSnapshot
{
    bool valid = false;
    double core0_percent = 0.0;
    double core1_percent = 0.0;
    double total_percent = 0.0;
};

static double clamp_cpu_percent(double value)
{
    if (value < 0.0)
    {
        return 0.0;
    }

    if (value > 100.0)
    {
        return 100.0;
    }

    return value;
}

static CpuUsageSnapshot read_cpu_usage_snapshot()
{
    CpuUsageSnapshot result;

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    const UBaseType_t task_count =
        uxTaskGetNumberOfTasks();

    if (task_count > CPU_STATS_MAX_TASKS)
    {
        ESP_LOGW(
            TAG,
            "CPU stats skipped: tasks=%u exceeds buffer=%u",
            static_cast<unsigned>(task_count),
            static_cast<unsigned>(CPU_STATS_MAX_TASKS)
        );

        return result;
    }

    configRUN_TIME_COUNTER_TYPE total_runtime = 0;

    const UBaseType_t captured =
        uxTaskGetSystemState(
            cpu_task_status,
            CPU_STATS_MAX_TASKS,
            &total_runtime
        );

    if (captured == 0)
    {
        ESP_LOGW(
            TAG,
            "CPU stats unavailable: uxTaskGetSystemState returned 0"
        );

        return result;
    }

    uint64_t idle0_runtime = 0;
    uint64_t idle1_runtime = 0;
    bool idle0_found = false;
    bool idle1_found = false;

    for (UBaseType_t index = 0; index < captured; ++index)
    {
        const TaskStatus_t& task =
            cpu_task_status[index];

        if (task.pcTaskName == nullptr)
        {
            continue;
        }

        if (std::strcmp(task.pcTaskName, "IDLE0") == 0)
        {
            idle0_runtime =
                static_cast<uint64_t>(
                    task.ulRunTimeCounter
                );

            idle0_found = true;
        }
        else if (std::strcmp(task.pcTaskName, "IDLE1") == 0)
        {
            idle1_runtime =
                static_cast<uint64_t>(
                    task.ulRunTimeCounter
                );

            idle1_found = true;
        }
    }

    /*
     * ESP32-S3 is dual-core in this project, therefore both idle tasks are
     * expected. If either is missing, do not report a misleading percentage.
     */
    if (!idle0_found || !idle1_found)
    {
        ESP_LOGW(
            TAG,
            "CPU stats unavailable: IDLE0=%s IDLE1=%s",
            idle0_found ? "found" : "missing",
            idle1_found ? "found" : "missing"
        );

        return result;
    }

    const uint64_t current_total =
        static_cast<uint64_t>(
            total_runtime
        );

    if (!cpu_runtime_baseline_valid)
    {
        previous_total_runtime = current_total;
        previous_idle0_runtime = idle0_runtime;
        previous_idle1_runtime = idle1_runtime;
        cpu_runtime_baseline_valid = true;

        return result;
    }

    /*
     * The configured run-time counter may wrap. If that happens, rebuild
     * the baseline instead of producing a bogus utilization value.
     */
    if (
        current_total <= previous_total_runtime ||
        idle0_runtime < previous_idle0_runtime ||
        idle1_runtime < previous_idle1_runtime
    )
    {
        previous_total_runtime = current_total;
        previous_idle0_runtime = idle0_runtime;
        previous_idle1_runtime = idle1_runtime;

        ESP_LOGW(
            TAG,
            "CPU run-time counter wrapped/reset; baseline restarted"
        );

        return result;
    }

    const uint64_t total_delta =
        current_total - previous_total_runtime;

    const uint64_t idle0_delta =
        idle0_runtime - previous_idle0_runtime;

    const uint64_t idle1_delta =
        idle1_runtime - previous_idle1_runtime;

    previous_total_runtime = current_total;
    previous_idle0_runtime = idle0_runtime;
    previous_idle1_runtime = idle1_runtime;

    if (total_delta == 0)
    {
        return result;
    }

    /*
     * Each idle task belongs to exactly one core. total_runtime is the
     * run-time-stat timer elapsed during the window, so each core had
     * total_delta units available in that same interval.
     */
    const double idle0_percent =
        (static_cast<double>(idle0_delta) * 100.0) /
        static_cast<double>(total_delta);

    const double idle1_percent =
        (static_cast<double>(idle1_delta) * 100.0) /
        static_cast<double>(total_delta);

    result.core0_percent =
        clamp_cpu_percent(
            100.0 - idle0_percent
        );

    result.core1_percent =
        clamp_cpu_percent(
            100.0 - idle1_percent
        );

    result.total_percent =
        (result.core0_percent +
         result.core1_percent) / 2.0;

    result.valid = true;
#else
    /*
     * Enable:
     * Component config -> FreeRTOS -> Kernel ->
     * Enable FreeRTOS to collect run time stats
     */
#endif

    return result;
}

static void log_system_diagnostics()
{
    const uint64_t current_us =
        static_cast<uint64_t>(
            esp_timer_get_time()
        );

    if (diagnostic_window_started_us == 0)
    {
        diagnostic_window_started_us = current_us;
        diagnostic_busy_us = 0;
        diagnostic_loop_count = 0;
        return;
    }

    const uint64_t elapsed_us =
        current_us - diagnostic_window_started_us;

    if (
        elapsed_us <
        static_cast<uint64_t>(
            SYSTEM_DIAGNOSTIC_INTERVAL_MS
        ) * 1000ULL
    )
    {
        return;
    }

    /*
     * This is the main-loop busy percentage, not total chip utilization.
     * It measures how much wall-clock time app_main spent doing work
     * versus sleeping/yielding during the diagnostic window.
     */
    double main_busy_percent = 0.0;

    if (elapsed_us > 0)
    {
        main_busy_percent =
            (static_cast<double>(diagnostic_busy_us) * 100.0) /
            static_cast<double>(elapsed_us);
    }

    const UBaseType_t stack_high_water =
        uxTaskGetStackHighWaterMark(nullptr);

    const CpuUsageSnapshot cpu =
        read_cpu_usage_snapshot();

    if (cpu.valid)
    {
        ESP_LOGI(
            TAG,
            "SYSTEM: state=%s CPU0=%.1f%% CPU1=%.1f%% TOTAL=%.1f%% "
            "main_busy=%.1f%% loops=%u stack_free_min=%u heap_free=%u",
            system_state_name(system_state),
            cpu.core0_percent,
            cpu.core1_percent,
            cpu.total_percent,
            main_busy_percent,
            static_cast<unsigned>(diagnostic_loop_count),
            static_cast<unsigned>(stack_high_water),
            static_cast<unsigned>(esp_get_free_heap_size())
        );
    }
    else
    {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
        ESP_LOGI(
            TAG,
            "SYSTEM: state=%s CPU=BASELINING main_busy=%.1f%% loops=%u "
            "stack_free_min=%u heap_free=%u",
            system_state_name(system_state),
            main_busy_percent,
            static_cast<unsigned>(diagnostic_loop_count),
            static_cast<unsigned>(stack_high_water),
            static_cast<unsigned>(esp_get_free_heap_size())
        );
#else
        ESP_LOGW(
            TAG,
            "SYSTEM: state=%s CPU=N/A main_busy=%.1f%% loops=%u "
            "stack_free_min=%u heap_free=%u | "
            "Enable CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS",
            system_state_name(system_state),
            main_busy_percent,
            static_cast<unsigned>(diagnostic_loop_count),
            static_cast<unsigned>(stack_high_water),
            static_cast<unsigned>(esp_get_free_heap_size())
        );
#endif
    }

    diagnostic_window_started_us = current_us;
    diagnostic_busy_us = 0;
    diagnostic_loop_count = 0;
}

/* =========================================================
 * Shared IR sensor
 *
 * GPIO7 is used for:
 * 1. Reminder acknowledgement
 * 2. Pomodoro double-tap gesture
 * 3. Healing dock-presence gate
 *
 * A stable-state filter prevents short gestures from changing
 * the healing dock state.
 * ========================================================= */

static constexpr gpio_num_t SHARED_IR_PIN = GPIO_NUM_7;
static constexpr bool SHARED_IR_ACTIVE_LOW = true;
static constexpr uint64_t DOCK_STABLE_TIME_MS = 1200;

static bool dock_filter_initialized = false;
static bool dock_candidate_state = false;
static bool dock_reported_state = false;
static uint64_t dock_candidate_since_ms = 0;

/*
 * IR-driven scheduling.
 *
 * Raw IR reads remain cheap and frequent. Heavier consumption/Pomodoro work
 * is only serviced when the sensor changes or while the dock stability filter
 * is still settling.
 */
static bool ir_activity_pending = false;
static bool dock_stability_pending = false;
static bool last_raw_ir_docked = false;
static uint64_t last_ir_poll_ms = 0;

static constexpr uint32_t IR_POLL_INTERVAL_MS = 20;
static constexpr uint32_t IDLE_DISPLAY_INTERVAL_MS = 250;
static constexpr uint32_t ACTION_LOG_DISPLAY_INTERVAL_MS = 50;
static constexpr uint32_t REMINDER_ENGINE_INTERVAL_MS = 100;
static constexpr uint32_t STATISTICS_INTERVAL_MS = 1000;

static uint64_t last_idle_display_ms = 0;
static uint64_t last_reminder_update_ms = 0;
static uint64_t last_statistics_update_ms = 0;


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


static bool read_shared_ir_docked()
{
    const int level = gpio_get_level(SHARED_IR_PIN);

    return SHARED_IR_ACTIVE_LOW
        ? level == 0
        : level == 1;
}

static void initialize_shared_ir_dock_state()
{
    const bool current_docked = read_shared_ir_docked();

    dock_filter_initialized = true;
    dock_candidate_state = current_docked;
    dock_reported_state = current_docked;
    last_raw_ir_docked = current_docked;
    dock_candidate_since_ms = application_millis();
    dock_stability_pending = false;
    ir_activity_pending = false;

    audio_manager_set_dock_state(current_docked);
    bottle_calibration_set_docked(current_docked);
    consumption_tracker_set_docked(current_docked);

    ESP_LOGI(
        TAG,
        "Initial shared IR dock state: %s",
        current_docked ? "DOCKED" : "UNDOCKED"
    );
}

static bool update_shared_ir_dock_state()
{
    const uint64_t current_ms = application_millis();

    if (
        current_ms - last_ir_poll_ms <
        IR_POLL_INTERVAL_MS
    )
    {
        return false;
    }

    last_ir_poll_ms = current_ms;

    const bool sampled_docked = read_shared_ir_docked();

    if (!dock_filter_initialized)
    {
        initialize_shared_ir_dock_state();
        return false;
    }

    if (sampled_docked != last_raw_ir_docked)
    {
        last_raw_ir_docked = sampled_docked;
        ir_activity_pending = true;
    }

    if (sampled_docked != dock_candidate_state)
    {
        dock_candidate_state = sampled_docked;
        dock_candidate_since_ms = current_ms;
        dock_stability_pending = true;
        return false;
    }

    if (dock_candidate_state == dock_reported_state)
    {
        dock_stability_pending = false;
        return false;
    }

    if (
        current_ms - dock_candidate_since_ms <
        DOCK_STABLE_TIME_MS
    )
    {
        dock_stability_pending = true;
        return false;
    }

    dock_reported_state = dock_candidate_state;
    dock_stability_pending = false;
    ir_activity_pending = true;

    action_log_show_bottle(
        dock_reported_state
            ? "detected"
            : "removed"
    );

    audio_manager_set_dock_state(dock_reported_state);
    bottle_calibration_set_docked(dock_reported_state);
    consumption_tracker_set_docked(dock_reported_state);

    ESP_LOGI(
        TAG,
        "Stable shared IR dock state: %s",
        dock_reported_state ? "DOCKED" : "UNDOCKED"
    );

    return true;
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


    "bottle_clean": {
      "enabled": true,
      "interval_days": 7,
      "hour": 17,
      "minute": 0,
      "display_ms": 15000,
      "require_ack": true
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
    "meditation": {
      "enabled": true,
      "tracks": [22]
    },
    "healing": {
      "enabled": true,
      "require_dock": true,
      "tracks": [60, 61, 62]
    },
    "healing_schedules": [
      {
        "enabled": true,
        "start_time": "06:00",
        "end_time": "07:00",
        "days": ["mon", "tue", "wed", "thu", "fri"]
      },
      {
        "enabled": true,
        "start_time": "12:30",
        "end_time": "13:00",
        "days": ["sat", "sun"]
      },
      {
        "enabled": true,
        "start_time": "21:00",
        "end_time": "21:30",
        "days": []
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

        case ReminderType::BOTTLE_CLEAN:
        {
            ESP_LOGI(TAG, "Showing bottle-clean reminder");
            display_show_bottle_clean_reminder();
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
    ir_activity_pending = true;

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
        "Bluetooth ready; advertising as %s",
        bluetooth_get_device_name()
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

    /* Mount SPIFFS and load the rolling 30-day statistics history. */
    const esp_err_t history_result = statistics_history_init();
    if (history_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Statistics history initialization failed: %s",
                 esp_err_to_name(history_result));
        /* Continue: today's NVS statistics still work. */
    }

    /* -----------------------------------------------------
     * Initialize HX711 scale and bottle calibration
     * ----------------------------------------------------- */

    const esp_err_t scale_result = scale_init();

    if (scale_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Scale initialization failed: %s",
            esp_err_to_name(scale_result)
        );
    }

    bottle_calibration_init();

    const esp_err_t consumption_result = consumption_tracker_init();
    if (consumption_result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Consumption tracker initialization failed: %s",
            esp_err_to_name(consumption_result)
        );
    }



  /* -----------------------------------------------------
     * Initialize Bluetooth
     * ----------------------------------------------------- */

    initialize_bluetooth();

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
        SHARED_IR_PIN;

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
    else
    {
        initialize_shared_ir_dock_state();
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
     * Initialize action log, Wi-Fi and OTA
     * ----------------------------------------------------- */

    action_log_init();

    const esp_err_t wifi_result = wifi_manager_init();
    if (wifi_result != ESP_OK)
    {
        ESP_LOGE(TAG, "Wi-Fi manager initialization failed: %s", esp_err_to_name(wifi_result));
    }

    const esp_err_t ota_result = ota_manager_init();
    if (ota_result != ESP_OK)
    {
        ESP_LOGE(TAG, "OTA manager initialization failed: %s", esp_err_to_name(ota_result));
    }

  

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

    state_entered_ms = application_millis();
    diagnostic_window_started_us =
        static_cast<uint64_t>(esp_timer_get_time());

    ESP_LOGI(
        TAG,
        "STATE: initial=%s",
        system_state_name(system_state)
    );

    while (true)
    {
        const uint64_t loop_started_us =
            static_cast<uint64_t>(
                esp_timer_get_time()
            );

        const time_t now =
            time(nullptr);

        /*
         * Lightweight common services.
         *
         * These remain active in every state because they maintain
         * user input, Action Log lifetime, audio/DFPlayer servicing,
         * dock detection, and BLE calibration requests.
         */
        action_log_update();

        acknowledgement_input_update();

        const bool dock_state_changed =
            update_shared_ir_dock_state();

        if (
            pomodoro_first_tap_pending ||
            ir_activity_pending
        )
        {
            update_pomodoro_double_tap();
        }

        audio_manager_update(now);

        /*
         * BLE callbacks queue calibration commands. Execute those requests
         * only from app_main so display/HX711 ownership remains deterministic.
         */
        if (bluetooth_take_bottle_calibration_cancel_request())
        {
            ESP_LOGI(
                TAG,
                "Processing BLE bottle calibration cancel request"
            );

            bottle_calibration_cancel();
        }

        if (bluetooth_take_bottle_calibration_start_request())
        {
            ESP_LOGI(
                TAG,
                "Processing BLE bottle calibration start request"
            );

            if (!bottle_calibration_start())
            {
                ESP_LOGE(
                    TAG,
                    "Bottle calibration could not start"
                );
            }
        }

        /*
         * Resolve the current state before state-specific work.
         */
        SystemState requested_state =
            determine_system_state();

        if (requested_state != system_state)
        {
            transition_system_state(
                requested_state,
                "priority evaluation"
            );
        }

        switch (system_state)
        {
            case SystemState::CALIBRATION:
            {
                /*
                 * Calibration owns HX711 processing and the full display.
                 * Normal reminders, Pomodoro rendering, consumption updates,
                 * and statistics rollover work are suspended.
                 */
                consumption_tracker_set_enabled(false);

                bottle_calibration_update();

                if (!bottle_calibration_is_active())
                {
                    consumption_tracker_set_enabled(true);

                    transition_system_state(
                        SystemState::IDLE,
                        "calibration finished"
                    );
                }

                break;
            }

            case SystemState::OTA:
            {
                /*
                 * OTA runs in its own task. Keep this main task focused on
                 * the Action Log / progress arc and essential common services.
                 *
                 * Do not run reminder scheduling, Pomodoro, consumption
                 * processing, or statistics maintenance while OTA is active.
                 */
                if (!reminder_engine_has_active_reminder())
                {
                    display_show_home_clock(now);
                }

                if (!ota_manager_is_busy())
                {
                    transition_system_state(
                        SystemState::IDLE,
                        "OTA finished"
                    );
                }

                break;
            }

            case SystemState::REMINDER:
            {
                /*
                 * The reminder callback already owns the reminder screen.
                 * Continue updating the reminder engine for timeout,
                 * acknowledgement, snooze, and completion behavior.
                 */
                reminder_engine_update(now);

                if (!reminder_engine_has_active_reminder())
                {
                    transition_system_state(
                        determine_system_state(),
                        "reminder finished"
                    );
                }

                break;
            }

            case SystemState::POMODORO:
            {
                const bool pomodoro_focus_active =
                    pomodoro_is_running() &&
                    pomodoro_get_state() ==
                        PomodoroState::FOCUS;

                reminder_engine_set_medication_only_activation(
                    pomodoro_focus_active
                );

                /*
                 * Keep reminder detection active during Pomodoro so the
                 * existing focus/break reminder rules remain unchanged.
                 */
                reminder_engine_update(now);

                if (reminder_engine_has_active_reminder())
                {
                    transition_system_state(
                        SystemState::REMINDER,
                        "reminder activated during Pomodoro"
                    );

                    break;
                }

                pomodoro_update();

                if (pomodoro_is_running())
                {
                    pomodoro_render_if_needed();
                }
                else
                {
                    transition_system_state(
                        SystemState::IDLE,
                        "Pomodoro stopped"
                    );
                }

                break;
            }

            case SystemState::CONSUMPTION:
            {
                /*
                 * Keep the consumption result screen as the display owner.
                 * Continue tracker processing so its timeout/state can finish.
                 */
                consumption_tracker_set_enabled(true);
                consumption_tracker_set_docked(dock_reported_state);
                consumption_tracker_update();

                if (consumption_tracker_screen_active())
                {
                    consumption_tracker_render_screen();
                }
                else
                {
                    transition_system_state(
                        determine_system_state(),
                        "consumption screen finished"
                    );
                }

                break;
            }

            case SystemState::IDLE:
            default:
            {
                const uint64_t current_ms =
                    application_millis();

                if (
                    current_ms - last_statistics_update_ms >=
                    STATISTICS_INTERVAL_MS
                )
                {
                    last_statistics_update_ms = current_ms;
                    user_statistics_update_day();
                }

                if (
                    current_ms - last_reminder_update_ms >=
                    REMINDER_ENGINE_INTERVAL_MS
                )
                {
                    last_reminder_update_ms = current_ms;

                    reminder_engine_set_medication_only_activation(false);
                    reminder_engine_update(now);
                }

                if (reminder_engine_has_active_reminder())
                {
                    transition_system_state(
                        SystemState::REMINDER,
                        "reminder activated"
                    );

                    break;
                }

                if (
                    ir_activity_pending ||
                    dock_state_changed ||
                    dock_stability_pending
                )
                {
                    consumption_tracker_set_enabled(true);
                    consumption_tracker_set_docked(dock_reported_state);
                    consumption_tracker_update();
                }

                requested_state =
                    determine_system_state();

                if (requested_state != SystemState::IDLE)
                {
                    ir_activity_pending = false;

                    transition_system_state(
                        requested_state,
                        "subsystem activated"
                    );

                    break;
                }

                const uint32_t display_interval_ms =
                    action_log_is_visible()
                        ? ACTION_LOG_DISPLAY_INTERVAL_MS
                        : IDLE_DISPLAY_INTERVAL_MS;

                if (
                    current_ms - last_idle_display_ms >=
                    display_interval_ms
                )
                {
                    last_idle_display_ms = current_ms;
                    display_show_home_clock(now);
                }

                ir_activity_pending = false;

                break;
            }
        }

        const uint64_t loop_finished_us =
            static_cast<uint64_t>(
                esp_timer_get_time()
            );

        if (loop_finished_us >= loop_started_us)
        {
            diagnostic_busy_us +=
                loop_finished_us - loop_started_us;
        }

        ++diagnostic_loop_count;

        log_system_diagnostics();

        vTaskDelay(
            pdMS_TO_TICKS(
                MAIN_LOOP_PERIOD_MS
            )
        );
    }
}