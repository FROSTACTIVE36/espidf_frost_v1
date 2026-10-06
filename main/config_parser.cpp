#include "config_parser.hpp"

#include <cstdio>
#include <cstring>
#include "pomodoro.hpp"
#include "audio_manager.hpp"

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

static int date_number(const ReminderDate& date)
{
    return date.year * 10000 + date.month * 100 + date.day;
}

static bool parse_date_range(const cJSON* object,
                             ReminderDate& start, ReminderDate& end)
{
    const char* start_text = get_string(object, "start_date");
    const char* end_text = get_string(object, "end_date");
    const auto valid_range_date = [](const char* text, ReminderDate& date) {
        if (text == nullptr || std::strlen(text) != 10 ||
            text[4] != '-' || text[7] != '-') return false;
        for (int i = 0; i < 10; ++i)
            if (i != 4 && i != 7 && (text[i] < '0' || text[i] > '9'))
                return false;
        if (!parse_date_string(text, date)) return false;
        const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
        int maximum = days[date.month - 1];
        if (date.month == 2 &&
            (date.year % 400 == 0 ||
             (date.year % 4 == 0 && date.year % 100 != 0)))
            maximum = 29;
        return date.day <= maximum;
    };
    return valid_range_date(start_text, start) &&
           valid_range_date(end_text, end) &&
           date_number(start) <= date_number(end);
}

static bool parse_reminder_time(const cJSON* object, ReminderTime& time)
{
    if (!cJSON_IsObject(object)) return false;
    const int hour = get_int(object, "h", -1);
    const int minute = get_int(object, "m", -1);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) return false;
    time.hour = static_cast<uint8_t>(hour);
    time.minute = static_cast<uint8_t>(minute);
    return true;
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

static ReminderConfig parse_standard_config(const cJSON* reminder_json)
{
    ReminderConfig config;
    config.enabled = get_bool(reminder_json, "enabled", false);
    config.display_ms = static_cast<uint32_t>(
        get_int(reminder_json, "display_ms", 10000));
    config.require_ack = get_bool(reminder_json, "require_ack", true);
    config.day_mask = parse_day_mask(reminder_json, "days");
    const bool valid_range = parse_date_range(
        reminder_json, config.start_date, config.end_date);
    parse_absolute_times(reminder_json, config);
    if (!valid_range || config.absolute_time_count == 0)
    {
        config.enabled = false;
        ESP_LOGW(TAG, "Standard reminder has no valid date range or times");
    }
    return config;
}

static BottleCleanConfig parse_bottle_clean_config(
    const cJSON* bottle_clean_json
)
{
    BottleCleanConfig config;

    config.enabled = get_bool(bottle_clean_json, "enabled", false);

    const bool valid_range = parse_date_range(
        bottle_clean_json, config.start_date, config.end_date);

    int interval_days = get_int(bottle_clean_json, "interval_days", 7);
    if (interval_days < 1) interval_days = 1;
    if (interval_days > 365) interval_days = 365;
    config.interval_days = static_cast<uint16_t>(interval_days);

    const bool valid_time = parse_reminder_time(
        cJSON_GetObjectItemCaseSensitive(bottle_clean_json, "time"),
        config.time);

    int display_ms = get_int(bottle_clean_json, "display_ms", 15000);
    if (display_ms < 0) display_ms = 0;
    config.display_ms = static_cast<uint32_t>(display_ms);

    config.require_ack = get_bool(bottle_clean_json, "require_ack", true);
    if (!valid_range || !valid_time)
    {
        config.enabled = false;
        ESP_LOGW(TAG, "Bottle-clean has no valid date range or time");
    }
    return config;
}

static MeditationConfig parse_meditation_config(const cJSON* meditation_json)
{
    MeditationConfig config;
    config.enabled = get_bool(meditation_json, "enabled", false);
    config.display_ms = static_cast<uint32_t>(
        get_int(meditation_json, "display_ms", 600000));
    config.require_ack = get_bool(meditation_json, "require_ack", true);
    config.day_mask = parse_day_mask(meditation_json, "days");
    const bool valid_range = parse_date_range(
        meditation_json, config.start_date, config.end_date);
    const cJSON* times = cJSON_GetObjectItemCaseSensitive(
        meditation_json, "times");
    if (cJSON_IsArray(times))
    {
        const cJSON* item = nullptr;
        cJSON_ArrayForEach(item, times)
        {
            if (config.time_count >= MAX_ABSOLUTE_TIMES) break;
            ReminderTime start, end;
            if (!parse_reminder_time(cJSON_GetObjectItemCaseSensitive(item, "start"), start) ||
                !parse_reminder_time(cJSON_GetObjectItemCaseSensitive(item, "end"), end))
            {
                continue;
            }
            const int start_minute = start.hour * 60 + start.minute;
            const int end_minute = end.hour * 60 + end.minute;
            // Meditation windows begin and end on the same calendar day.
            if (end_minute <= start_minute) continue;
            config.times[config.time_count++] = {start, end};
        }
    }
    if (!valid_range || config.time_count == 0)
    {
        config.enabled = false;
        ESP_LOGW(TAG, "Meditation has no valid date range or windows");
    }
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

    config.require_ack = get_bool(custom_json, "require_ack", true);
    config.display_ms = static_cast<uint32_t>(
        get_int(custom_json, "display_ms", 60000));
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
        event = {};

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

        event.day_mask = parse_day_mask(event_json, "days");
        const bool valid_range = parse_date_range(
            event_json, event.start_date, event.end_date);
        const cJSON* times = cJSON_GetObjectItemCaseSensitive(event_json, "times");
        if (cJSON_IsArray(times))
        {
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, times)
            {
                if (event.time_count >= MAX_ABSOLUTE_TIMES) break;
                ReminderTime time;
                if (!parse_reminder_time(item, time)) continue;
                event.times[event.time_count++] = time;
            }
        }
        if (!valid_range || event.time_count == 0)
        {
            ESP_LOGW(TAG, "Skipping custom event with invalid date range or times");
            continue;
        }

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
 * Audio configuration parser
 * ========================================================= */

static AudioPlaylistConfig parse_audio_playlist(
    const cJSON* playlist_json,
    const AudioPlaylistConfig& defaults
)
{
    AudioPlaylistConfig config = defaults;

    if (!cJSON_IsObject(playlist_json))
    {
        return config;
    }

    config.enabled =
        get_bool(
            playlist_json,
            "enabled",
            config.enabled
        );

    const cJSON* tracks =
        cJSON_GetObjectItemCaseSensitive(
            playlist_json,
            "tracks"
        );

    if (!cJSON_IsArray(tracks))
    {
        return config;
    }

    /*
     * Supplying a tracks array replaces the previous/default list.
     */
    config.track_count = 0;

    const cJSON* track_json = nullptr;

    cJSON_ArrayForEach(track_json, tracks)
    {
        if (
            config.track_count >=
            MAX_AUDIO_PLAYLIST_TRACKS
        )
        {
            break;
        }

        if (!cJSON_IsNumber(track_json))
        {
            continue;
        }

        const int track = track_json->valueint;

        if (track < 1 || track > 65535)
        {
            continue;
        }

        config.tracks[config.track_count] =
            static_cast<uint16_t>(track);

        ++config.track_count;
    }

    return config;
}

static void parse_hhmm(
    const cJSON* object,
    const char* key,
    uint8_t& hour,
    uint8_t& minute
)
{
    const cJSON* value =
        cJSON_GetObjectItemCaseSensitive(
            object,
            key
        );

    if (!cJSON_IsString(value) || value->valuestring == nullptr)
    {
        return;
    }

    int parsed_hour = -1;
    int parsed_minute = -1;

    if (
        sscanf(
            value->valuestring,
            "%d:%d",
            &parsed_hour,
            &parsed_minute
        ) != 2
    )
    {
        return;
    }

    if (
        parsed_hour < 0 ||
        parsed_hour > 23 ||
        parsed_minute < 0 ||
        parsed_minute > 59
    )
    {
        return;
    }

    hour = static_cast<uint8_t>(parsed_hour);
    minute = static_cast<uint8_t>(parsed_minute);
}

static AudioManagerConfig parse_audio_config(
    const cJSON* audio_json
)
{
    AudioManagerConfig config =
        audio_manager_get_config();

    if (!cJSON_IsObject(audio_json))
    {
        return config;
    }

    int volume =
        get_int(
            audio_json,
            "volume",
            config.volume
        );

    if (volume < 0)
    {
        volume = 0;
    }
    else if (volume > 30)
    {
        volume = 30;
    }

    config.volume =
        static_cast<uint8_t>(volume);

    config.pomodoro =
        parse_audio_playlist(
            cJSON_GetObjectItemCaseSensitive(
                audio_json,
                "pomodoro"
            ),
            config.pomodoro
        );

    config.meditation =
        parse_audio_playlist(
            cJSON_GetObjectItemCaseSensitive(
                audio_json,
                "meditation"
            ),
            config.meditation
        );

    /*
     * Accept both "healing" and the common misspelling "heling".
     */
    const cJSON* healing =
        cJSON_GetObjectItemCaseSensitive(
            audio_json,
            "healing"
        );

    if (!cJSON_IsObject(healing))
    {
        healing =
            cJSON_GetObjectItemCaseSensitive(
                audio_json,
                "heling"
            );
    }

    config.healing =
        parse_audio_playlist(
            healing,
            config.healing
        );

    if (cJSON_IsObject(healing))
    {
        config.healing_require_dock =
            get_bool(
                healing,
                "require_dock",
                config.healing_require_dock
            );
    }

    /*
     * Preferred format:
     *
     * "healing_schedules": [
     *   {
     *     "enabled": true,
     *     "start_time": "06:00",
     *     "end_time": "07:00"
     *   }
     * ]
     *
     * The older single-object key "healing_schedule" is also accepted.
     */
    const cJSON* healing_schedules =
        cJSON_GetObjectItemCaseSensitive(
            audio_json,
            "healing_schedules"
        );

    if (cJSON_IsArray(healing_schedules))
    {
        config.healing_schedule_count = 0;

        const cJSON* schedule_json = nullptr;

        cJSON_ArrayForEach(
            schedule_json,
            healing_schedules
        )
        {
            if (
                config.healing_schedule_count >=
                MAX_HEALING_SCHEDULES
            )
            {
                break;
            }

            if (!cJSON_IsObject(schedule_json))
            {
                continue;
            }

            HealingScheduleConfig schedule = {};

            schedule.enabled =
                get_bool(
                    schedule_json,
                    "enabled",
                    true
                );

            schedule.day_mask =
                parse_day_mask(
                    schedule_json,
                    "days"
                );

            parse_hhmm(
                schedule_json,
                "start_time",
                schedule.start_hour,
                schedule.start_minute
            );

            parse_hhmm(
                schedule_json,
                "end_time",
                schedule.end_hour,
                schedule.end_minute
            );

            config.healing_schedules[
                config.healing_schedule_count
            ] = schedule;

            ++config.healing_schedule_count;
        }
    }
    else
    {
        const cJSON* healing_schedule =
            cJSON_GetObjectItemCaseSensitive(
                audio_json,
                "healing_schedule"
            );

        if (cJSON_IsObject(healing_schedule))
        {
            HealingScheduleConfig schedule = {};

            schedule.enabled =
                get_bool(
                    healing_schedule,
                    "enabled",
                    true
                );

            schedule.day_mask =
                parse_day_mask(
                    healing_schedule,
                    "days"
                );

            parse_hhmm(
                healing_schedule,
                "start_time",
                schedule.start_hour,
                schedule.start_minute
            );

            parse_hhmm(
                healing_schedule,
                "end_time",
                schedule.end_hour,
                schedule.end_minute
            );

            config.healing_schedules[0] =
                schedule;

            config.healing_schedule_count = 1;
        }
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

    config.day_mask = parse_day_mask(pomodoro_json, "days");
    const bool valid_range = parse_date_range(
        pomodoro_json, config.start_date, config.end_date);
    if (!valid_range)
    {
        ESP_LOGW(TAG, "Disabling Pomodoro: invalid date range");
        config.enabled = false;
    }

    if (config.focus_min == 0)
    {
        config.focus_min = 1;
    }

    if (config.break_min == 0)
    {
        config.break_min = 1;
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

            ReminderTime start = {};
            const int cycles = get_int(lap_json, "cycles", -1);
            if (!parse_reminder_time(
                    cJSON_GetObjectItemCaseSensitive(lap_json, "start"), start) ||
                cycles < 1 || cycles > 255)
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

            lap.start_hour = start.hour;
            lap.start_minute = start.minute;
            lap.cycles = static_cast<uint8_t>(cycles);

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

    const cJSON* bottle_clean =
        cJSON_GetObjectItemCaseSensitive(
            reminders,
            "bottle_clean"
        );

    if (cJSON_IsObject(bottle_clean))
    {
        reminder_engine_set_bottle_clean_config(
            parse_bottle_clean_config(bottle_clean)
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
     * Audio playlists are configured at the root level.
     *
     * "audio": {
     *   "volume": 20,
     *   "pomodoro": {"enabled": true, "tracks": [50, 51]},
     *   "healing":  {"enabled": true, "tracks": [60, 61]}
     * }
     */
    const cJSON* audio =
        cJSON_GetObjectItemCaseSensitive(
            root,
            "audio"
        );

    if (cJSON_IsObject(audio))
    {
        audio_manager_set_config(
            parse_audio_config(audio)
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
