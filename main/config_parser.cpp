#include "config_parser.hpp"

#include <cstdio>
#include <cstring>
#include "pomodoro.hpp"

#include "cJSON.h"
#include "esp_log.h"

#include "reminder_engine.hpp"

static const char* TAG = "CONFIG_PARSER";

static bool set_error(
    char* destination,
    std::size_t destination_size,
    const char* message
)
{
    if (
        destination != nullptr &&
        destination_size > 0
    )
    {
        std::snprintf(
            destination,
            destination_size,
            "%s",
            message != nullptr
                ? message
                : "Configuration error"
        );
    }

    return false;
}

static bool get_bool(
    const cJSON* object,
    const char* name,
    bool default_value
)
{
    const cJSON* item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            name
        );

    if (!cJSON_IsBool(item))
    {
        return default_value;
    }

    return cJSON_IsTrue(item);
}

static int get_int(
    const cJSON* object,
    const char* name,
    int default_value
)
{
    const cJSON* item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            name
        );

    if (!cJSON_IsNumber(item))
    {
        return default_value;
    }

    return item->valueint;
}

static const char* get_string(
    const cJSON* object,
    const char* name,
    const char* default_value = nullptr
)
{
    const cJSON* item =
        cJSON_GetObjectItemCaseSensitive(
            object,
            name
        );

    if (
        !cJSON_IsString(item) ||
        item->valuestring == nullptr
    )
    {
        return default_value;
    }

    return item->valuestring;
}

static void copy_string(
    char* destination,
    std::size_t destination_size,
    const char* source
)
{
    if (
        destination == nullptr ||
        destination_size == 0
    )
    {
        return;
    }

    std::snprintf(
        destination,
        destination_size,
        "%s",
        source != nullptr ? source : ""
    );
}

static int day_name_to_index(
    const char* day_name
)
{
    if (day_name == nullptr)
    {
        return -1;
    }

    if (std::strcmp(day_name, "sun") == 0)
    {
        return 0;
    }

    if (std::strcmp(day_name, "mon") == 0)
    {
        return 1;
    }

    if (std::strcmp(day_name, "tue") == 0)
    {
        return 2;
    }

    if (std::strcmp(day_name, "wed") == 0)
    {
        return 3;
    }

    if (std::strcmp(day_name, "thu") == 0)
    {
        return 4;
    }

    if (std::strcmp(day_name, "fri") == 0)
    {
        return 5;
    }

    if (std::strcmp(day_name, "sat") == 0)
    {
        return 6;
    }

    return -1;
}

static uint8_t parse_day_mask(
    const cJSON* object,
    const char* field_name
)
{
    const cJSON* days =
        cJSON_GetObjectItemCaseSensitive(
            object,
            field_name
        );

    if (
        days == nullptr ||
        cJSON_IsNull(days)
    )
    {
        return 0;
    }

    if (!cJSON_IsArray(days))
    {
        return 0;
    }

    uint8_t mask = 0;

    const cJSON* day = nullptr;

    cJSON_ArrayForEach(day, days)
    {
        if (
            !cJSON_IsString(day) ||
            day->valuestring == nullptr
        )
        {
            continue;
        }

        const int index =
            day_name_to_index(
                day->valuestring
            );

        if (index >= 0)
        {
            mask |= static_cast<uint8_t>(
                1U << index
            );
        }
    }

    return mask;
}

static bool parse_date_string(
    const char* value,
    ReminderDate& date
)
{
    if (value == nullptr)
    {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;

    if (
        std::sscanf(
            value,
            "%d-%d-%d",
            &year,
            &month,
            &day
        ) != 3
    )
    {
        return false;
    }

    if (
        year < 1970 ||
        month < 1 ||
        month > 12 ||
        day < 1 ||
        day > 31
    )
    {
        return false;
    }

    date.year = year;
    date.month =
        static_cast<uint8_t>(month);

    date.day =
        static_cast<uint8_t>(day);

    return true;
}

static ReminderMode parse_mode(
    const cJSON* object
)
{
    const char* mode =
        get_string(
            object,
            "mode",
            "interval"
        );

    if (
        mode != nullptr &&
        std::strcmp(mode, "absolute") == 0
    )
    {
        return ReminderMode::ABSOLUTE;
    }

    return ReminderMode::INTERVAL;
}

static void parse_absolute_times(
    const cJSON* reminder_json,
    ReminderConfig& config
)
{
    const cJSON* abs_object =
        cJSON_GetObjectItemCaseSensitive(
            reminder_json,
            "abs"
        );

    if (!cJSON_IsObject(abs_object))
    {
        return;
    }

    const cJSON* times =
        cJSON_GetObjectItemCaseSensitive(
            abs_object,
            "times"
        );

    if (!cJSON_IsArray(times))
    {
        return;
    }

    config.absolute_time_count = 0;

    const cJSON* time_item = nullptr;

    cJSON_ArrayForEach(time_item, times)
    {
        if (
            config.absolute_time_count >=
            MAX_ABSOLUTE_TIMES
        )
        {
            break;
        }

        if (!cJSON_IsObject(time_item))
        {
            continue;
        }

        const int hour =
            get_int(time_item, "h", -1);

        const int minute =
            get_int(time_item, "m", -1);

        if (
            hour < 0 ||
            hour > 23 ||
            minute < 0 ||
            minute > 59
        )
        {
            continue;
        }

        ReminderTime& destination =
            config.absolute_times[
                config.absolute_time_count
            ];

        destination.hour =
            static_cast<uint8_t>(hour);

        destination.minute =
            static_cast<uint8_t>(minute);

        ++config.absolute_time_count;
    }
}

static ReminderConfig parse_standard_config(
    const cJSON* reminder_json
)
{
    ReminderConfig config;

    config.enabled =
        get_bool(
            reminder_json,
            "enabled",
            false
        );

    config.mode =
        parse_mode(reminder_json);

    config.interval_ms =
        static_cast<uint32_t>(
            get_int(
                reminder_json,
                "interval_ms",
                0
            )
        );

    config.display_ms =
        static_cast<uint32_t>(
            get_int(
                reminder_json,
                "display_ms",
                10000
            )
        );

    config.require_ack =
        get_bool(
            reminder_json,
            "require_ack",
            false
        );
config.start_hour =
        static_cast<uint8_t>(
            get_int(
                reminder_json,
                "start_hour",
                0
            )
        );

    config.start_minute =
        static_cast<uint8_t>(
            get_int(
                reminder_json,
                "start_min",
                0
            )
        );

    config.end_hour =
        static_cast<uint8_t>(
            get_int(
                reminder_json,
                "end_hour",
                23
            )
        );

    config.end_minute =
        static_cast<uint8_t>(
            get_int(
                reminder_json,
                "end_min",
                59
            )
        );

    config.day_mask =
        parse_day_mask(
            reminder_json,
            "days"
        );

    parse_absolute_times(
        reminder_json,
        config
    );

    return config;
}

static MeditationConfig parse_meditation_config(
    const cJSON* meditation_json
)
{
    MeditationConfig config;

    config.enabled =
        get_bool(
            meditation_json,
            "enabled",
            false
        );

    config.start_hour =
        static_cast<uint8_t>(
            get_int(
                meditation_json,
                "sh",
                6
            )
        );

    config.start_minute =
        static_cast<uint8_t>(
            get_int(
                meditation_json,
                "sm",
                0
            )
        );

    config.end_hour =
        static_cast<uint8_t>(
            get_int(
                meditation_json,
                "eh",
                7
            )
        );

    config.end_minute =
        static_cast<uint8_t>(
            get_int(
                meditation_json,
                "em",
                0
            )
        );

    const int display_seconds =
        get_int(
            meditation_json,
            "display_sec",
            600
        );

    config.display_ms =
        static_cast<uint32_t>(
            display_seconds
        ) * 1000UL;

    config.require_ack =
        get_bool(
            meditation_json,
            "require_ack",
            false
        );
config.day_mask =
        parse_day_mask(
            meditation_json,
            "days"
        );

    return config;
}

static MedicationConfig parse_medication_config(
    const cJSON* medication_json
)
{
    MedicationConfig config;

    config.enabled =
        get_bool(
            medication_json,
            "enabled",
            false
        );

    config.require_ack =
        get_bool(
            medication_json,
            "require_ack",
            true
        );

    config.snooze_min =
        static_cast<uint16_t>(
            get_int(
                medication_json,
                "snooze_min",
                10
            )
        );

    config.display_ms =
        static_cast<uint32_t>(
            get_int(
                medication_json,
                "display_ms",
                60000
            )
        );

    const cJSON* medicines =
        cJSON_GetObjectItemCaseSensitive(
            medication_json,
            "medicines"
        );

    /*
     * Supports both:
     *
     * "medication": {
     *    "medicines": [...]
     * }
     *
     * and older:
     *
     * "medication": [...]
     */
    if (!cJSON_IsArray(medicines))
    {
        if (cJSON_IsArray(medication_json))
        {
            medicines = medication_json;
            config.enabled = true;
        }
        else
        {
            return config;
        }
    }

    const cJSON* medicine_json = nullptr;

    cJSON_ArrayForEach(
        medicine_json,
        medicines
    )
    {
        if (
            config.medicine_count >=
            MAX_MEDICINES
        )
        {
            break;
        }

        if (!cJSON_IsObject(medicine_json))
        {
            continue;
        }

        MedicationItem& medicine =
            config.medicines[
                config.medicine_count
            ];

        medicine.enabled =
            get_bool(
                medicine_json,
                "enabled",
                true
            );

        copy_string(
            medicine.id,
            sizeof(medicine.id),
            get_string(
                medicine_json,
                "id",
                ""
            )
        );

        copy_string(
            medicine.label,
            sizeof(medicine.label),
            get_string(
                medicine_json,
                "label",
                "Medication"
            )
        );

        parse_date_string(
            get_string(
                medicine_json,
                "start"
            ),
            medicine.start_date
        );

        parse_date_string(
            get_string(
                medicine_json,
                "end"
            ),
            medicine.end_date
        );

        medicine.day_mask =
            parse_day_mask(
                medicine_json,
                "days"
            );

        medicine.text_x =
            static_cast<int16_t>(
                get_int(
                    medicine_json,
                    "text_x",
                    120
                )
            );

        medicine.text_y =
            static_cast<int16_t>(
                get_int(
                    medicine_json,
                    "text_y",
                    160
                )
            );

        medicine.text_size =
            static_cast<uint8_t>(
                get_int(
                    medicine_json,
                    "text_size",
                    1
                )
            );

        medicine.text_color =
            static_cast<uint16_t>(
                get_int(
                    medicine_json,
                    "text_color",
                    65535
                )
            );

        medicine.text_align =
            static_cast<uint8_t>(
                get_int(
                    medicine_json,
                    "text_align",
                    1
                )
            );

        medicine.text_width =
            static_cast<uint16_t>(
                get_int(
                    medicine_json,
                    "text_width",
                    180
                )
            );

        const cJSON* doses =
            cJSON_GetObjectItemCaseSensitive(
                medicine_json,
                "doses"
            );

        if (cJSON_IsArray(doses))
        {
            const cJSON* dose_json = nullptr;

            cJSON_ArrayForEach(
                dose_json,
                doses
            )
            {
                if (
                    medicine.dose_count >=
                    MAX_DOSES_PER_MEDICINE
                )
                {
                    break;
                }

                if (!cJSON_IsObject(dose_json))
                {
                    continue;
                }

                const int hour =
                    get_int(
                        dose_json,
                        "h",
                        -1
                    );

                const int minute =
                    get_int(
                        dose_json,
                        "m",
                        -1
                    );

                if (
                    hour < 0 ||
                    hour > 23 ||
                    minute < 0 ||
                    minute > 59
                )
                {
                    continue;
                }

                ReminderTime& dose =
                    medicine.doses[
                        medicine.dose_count
                    ];

                dose.hour =
                    static_cast<uint8_t>(hour);

                dose.minute =
                    static_cast<uint8_t>(minute);

                ++medicine.dose_count;
            }
        }

        ++config.medicine_count;
    }

    return config;
}

static CustomReminderConfig parse_custom_config(
    const cJSON* custom_json
)
{
    CustomReminderConfig config;

    config.enabled =
        get_bool(
            custom_json,
            "enabled",
            false
        );

    config.require_ack =
        get_bool(
            custom_json,
            "require_ack",
            true
        );
const cJSON* events =
        cJSON_GetObjectItemCaseSensitive(
            custom_json,
            "events"
        );

    if (!cJSON_IsArray(events))
    {
        return config;
    }

    const cJSON* event_json = nullptr;

    cJSON_ArrayForEach(
        event_json,
        events
    )
    {
        if (
            config.event_count >=
            MAX_CUSTOM_EVENTS
        )
        {
            break;
        }

        if (!cJSON_IsObject(event_json))
        {
            continue;
        }

        CustomEvent& event =
            config.events[
                config.event_count
            ];

        event.enabled =
            get_bool(
                event_json,
                "enabled",
                true
            );

        copy_string(
            event.id,
            sizeof(event.id),
            get_string(
                event_json,
                "id",
                ""
            )
        );

        copy_string(
            event.label,
            sizeof(event.label),
            get_string(
                event_json,
                "label",
                "Reminder"
            )
        );

        const char* type =
            get_string(
                event_json,
                "type",
                "recurring"
            );

        if (
            type != nullptr &&
            std::strcmp(type, "absolute") == 0
        )
        {
            event.type =
                CustomEventType::ABSOLUTE;
        }
        else
        {
            event.type =
                CustomEventType::RECURRING;
        }

        event.hour =
            static_cast<uint8_t>(
                get_int(
                    event_json,
                    "h",
                    0
                )
            );

        event.minute =
            static_cast<uint8_t>(
                get_int(
                    event_json,
                    "m",
                    0
                )
            );

        event.display_ms =
            static_cast<uint32_t>(
                get_int(
                    event_json,
                    "show_ms",
                    60000
                )
            );

        event.day_mask =
            parse_day_mask(
                event_json,
                "days"
            );

        parse_date_string(
            get_string(
                event_json,
                "date"
            ),
            event.date
        );

        event.text_x =
            static_cast<int16_t>(
                get_int(
                    event_json,
                    "text_x",
                    120
                )
            );

        event.text_y =
            static_cast<int16_t>(
                get_int(
                    event_json,
                    "text_y",
                    160
                )
            );

        event.text_size =
            static_cast<uint8_t>(
                get_int(
                    event_json,
                    "text_size",
                    1
                )
            );

        event.text_color =
            static_cast<uint16_t>(
                get_int(
                    event_json,
                    "text_color",
                    65535
                )
            );

        event.text_align =
            static_cast<uint8_t>(
                get_int(
                    event_json,
                    "text_align",
                    1
                )
            );

        event.text_width =
            static_cast<uint16_t>(
                get_int(
                    event_json,
                    "text_width",
                    180
                )
            );

        ++config.event_count;
    }

    return config;
}

/* =========================================================
 * Pomodoro configuration parser
 * ========================================================= */

static PomodoroCounterStyle parse_pomodoro_counter_style(
    const cJSON* object,
    const PomodoroCounterStyle& defaults
)
{
    PomodoroCounterStyle style =
        defaults;

    if (!cJSON_IsObject(object))
    {
        return style;
    }

    style.x =
        static_cast<int16_t>(
            get_int(
                object,
                "x",
                style.x
            )
        );

    style.y =
        static_cast<int16_t>(
            get_int(
                object,
                "y",
                style.y
            )
        );

    style.text_size =
        static_cast<uint8_t>(
            get_int(
                object,
                "text_size",
                style.text_size
            )
        );

    style.text_color =
        static_cast<uint16_t>(
            get_int(
                object,
                "text_color",
                style.text_color
            )
        );

    style.text_align =
        static_cast<uint8_t>(
            get_int(
                object,
                "text_align",
                style.text_align
            )
        );

    if (style.text_align > 2)
    {
        style.text_align = 1;
    }

    if (style.text_size == 0)
    {
        style.text_size = 1;
    }

    return style;
}

static PomodoroConfig parse_pomodoro_config(
    const cJSON* pomodoro_json
)
{
    PomodoroConfig config;

    if (!cJSON_IsObject(pomodoro_json))
    {
        return config;
    }

    config.enabled =
        get_bool(
            pomodoro_json,
            "enabled",
            false
        );

    config.focus_min =
        static_cast<uint16_t>(
            get_int(
                pomodoro_json,
                "focus_min",
                25
            )
        );

    config.break_min =
        static_cast<uint16_t>(
            get_int(
                pomodoro_json,
                "break_min",
                5
            )
        );

    config.cycles =
        static_cast<uint8_t>(
            get_int(
                pomodoro_json,
                "cycles",
                4
            )
        );

    config.auto_start_break =
        get_bool(
            pomodoro_json,
            "auto_start_break",
            true
        );

    config.auto_start_focus =
        get_bool(
            pomodoro_json,
            "auto_start_focus",
            true
        );

    if (config.focus_min == 0)
    {
        config.focus_min = 1;
    }

    if (config.break_min == 0)
    {
        config.break_min = 1;
    }

    if (config.cycles == 0)
    {
        config.cycles = 1;
    }

    const cJSON* focus_counter =
        cJSON_GetObjectItemCaseSensitive(
            pomodoro_json,
            "focus_counter"
        );

    const cJSON* break_counter =
        cJSON_GetObjectItemCaseSensitive(
            pomodoro_json,
            "break_counter"
        );

    config.focus_counter =
        parse_pomodoro_counter_style(
            focus_counter,
            config.focus_counter
        );

    config.break_counter =
        parse_pomodoro_counter_style(
            break_counter,
            config.break_counter
        );

    config.lap_mode_enabled =
        get_bool(
            pomodoro_json,
            "lap_mode_enabled",
            false
        );

    const cJSON* laps =
        cJSON_GetObjectItemCaseSensitive(
            pomodoro_json,
            "laps"
        );

    if (cJSON_IsArray(laps))
    {
        const cJSON* lap_json = nullptr;

        cJSON_ArrayForEach(
            lap_json,
            laps
        )
        {
            if (
                config.lap_count >=
                MAX_POMODORO_LAPS
            )
            {
                break;
            }

            if (!cJSON_IsObject(lap_json))
            {
                continue;
            }

            const int start_hour =
                get_int(
                    lap_json,
                    "sh",
                    get_int(
                        lap_json,
                        "start_hour",
                        -1
                    )
                );

            const int start_minute =
                get_int(
                    lap_json,
                    "sm",
                    get_int(
                        lap_json,
                        "start_min",
                        -1
                    )
                );

            const int end_hour =
                get_int(
                    lap_json,
                    "eh",
                    get_int(
                        lap_json,
                        "end_hour",
                        -1
                    )
                );

            const int end_minute =
                get_int(
                    lap_json,
                    "em",
                    get_int(
                        lap_json,
                        "end_min",
                        -1
                    )
                );

            if (
                start_hour < 0 ||
                start_hour > 23 ||
                start_minute < 0 ||
                start_minute > 59 ||
                end_hour < 0 ||
                end_hour > 23 ||
                end_minute < 0 ||
                end_minute > 59
            )
            {
                ESP_LOGW(
                    TAG,
                    "Ignoring invalid Pomodoro lap"
                );

                continue;
            }

            PomodoroLap& lap =
                config.laps[
                    config.lap_count
                ];

            lap.enabled =
                get_bool(
                    lap_json,
                    "enabled",
                    true
                );

            lap.start_hour =
                static_cast<uint8_t>(
                    start_hour
                );

            lap.start_minute =
                static_cast<uint8_t>(
                    start_minute
                );

            lap.end_hour =
                static_cast<uint8_t>(
                    end_hour
                );

            lap.end_minute =
                static_cast<uint8_t>(
                    end_minute
                );

            ++config.lap_count;
        }
    }

    return config;
}

bool reminder_config_parse_and_apply(
    const char* json_text,
    char* error_message,
    std::size_t error_message_size
)
{
    if (json_text == nullptr)
    {
        return set_error(
            error_message,
            error_message_size,
            "JSON text is null"
        );
    }

    cJSON* root =
        cJSON_Parse(json_text);

    if (root == nullptr)
    {
        return set_error(
            error_message,
            error_message_size,
            "Invalid JSON"
        );
    }

    const cJSON* reminders =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "reminders"
        );

    if (!cJSON_IsObject(reminders))
    {
        cJSON_Delete(root);

        return set_error(
            error_message,
            error_message_size,
            "Missing reminders object"
        );
    }

    struct StandardMapping
    {
        const char* name;
        ReminderType type;
    };

    static constexpr StandardMapping mappings[] =
    {
        {
            "hydration",
            ReminderType::HYDRATION
        },
        {
            "stretch",
            ReminderType::STRETCH
        },
        {
            "eye",
            ReminderType::EYE
        },
        {
            "walk",
            ReminderType::WALK
        }
    };

    for (const StandardMapping& mapping : mappings)
    {
        const cJSON* item =
            cJSON_GetObjectItemCaseSensitive(
                reminders,
                mapping.name
            );

        if (!cJSON_IsObject(item))
        {
            continue;
        }

        reminder_engine_set_config(
            mapping.type,
            parse_standard_config(item)
        );
    }

    const cJSON* meditation =
        cJSON_GetObjectItemCaseSensitive(
            reminders,
            "meditation"
        );

    if (cJSON_IsObject(meditation))
    {
        reminder_engine_set_meditation_config(
            parse_meditation_config(
                meditation
            )
        );
    }

    const cJSON* medication =
        cJSON_GetObjectItemCaseSensitive(
            reminders,
            "medication"
        );

    if (
        cJSON_IsObject(medication) ||
        cJSON_IsArray(medication)
    )
    {
        reminder_engine_set_medication_config(
            parse_medication_config(
                medication
            )
        );
    }

    const cJSON* custom =
        cJSON_GetObjectItemCaseSensitive(
            reminders,
            "custom"
        );

    if (cJSON_IsObject(custom))
    {
        reminder_engine_set_custom_config(
            parse_custom_config(custom)
        );
    }



        /*
     * Pomodoro is at the root level, not inside reminders.
     */
    const cJSON* pomodoro =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "pomodoro"
        );

    if (!cJSON_IsObject(pomodoro))
    {
        /*
         * Backward compatibility with the Arduino JSON schema.
         */
        pomodoro =
            cJSON_GetObjectItemCaseSensitive(
                root,
                "pomo"
            );
    }

    if (cJSON_IsObject(pomodoro))
    {
        pomodoro_set_config(
            parse_pomodoro_config(
                pomodoro
            )
        );
    }

    cJSON_Delete(root);

    if (
        error_message != nullptr &&
        error_message_size > 0
    )
    {
        error_message[0] = '\0';
    }

    ESP_LOGI(
        TAG,
        "Reminder configuration applied"
    );

    return true;
}