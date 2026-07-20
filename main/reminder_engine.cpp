#include "reminder_engine.hpp"

#include <array>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "REMINDER_ENGINE";

static constexpr std::size_t STANDARD_REMINDER_COUNT = 4;
static constexpr std::size_t REMINDER_QUEUE_SIZE = 16;

struct StandardReminderRuntime
{
    uint64_t last_trigger_ms = 0;

    int last_absolute_year = -1;
    int last_absolute_year_day = -1;
    int last_absolute_minute = -1;
};

struct MeditationRuntime
{
    int last_trigger_year = -1;
    int last_trigger_year_day = -1;
};

struct MedicationDoseRuntime
{
    int last_trigger_year = -1;
    int last_trigger_year_day = -1;
    int last_trigger_minute = -1;
};

struct CustomEventRuntime
{
    int last_trigger_year = -1;
    int last_trigger_year_day = -1;
    int last_trigger_minute = -1;
};

struct QueuedReminder
{
    ReminderType type = ReminderType::HYDRATION;

    int item_index = -1;
    int schedule_index = -1;

    uint32_t display_ms = 0;

    bool require_ack = false;
    uint16_t snooze_min = 0;
};

struct SnoozedReminder
{
    bool valid = false;

    QueuedReminder reminder = {};

    uint64_t due_ms = 0;
};

static std::array<
    ReminderConfig,
    STANDARD_REMINDER_COUNT
> standard_configs;

static std::array<
    StandardReminderRuntime,
    STANDARD_REMINDER_COUNT
> standard_runtime;

static MeditationConfig meditation_config;
static MeditationRuntime meditation_runtime;

static MedicationConfig medication_config;

static MedicationDoseRuntime
medication_runtime[MAX_MEDICINES][MAX_DOSES_PER_MEDICINE];

static CustomReminderConfig custom_config;

static CustomEventRuntime
custom_runtime[MAX_CUSTOM_EVENTS];

static ActiveReminder active_reminder;

static ReminderTriggerCallback trigger_callback = nullptr;
static ReminderFinishedCallback finished_callback = nullptr;

static QueuedReminder reminder_queue[REMINDER_QUEUE_SIZE];

static std::size_t queue_head = 0;
static std::size_t queue_tail = 0;
static std::size_t queue_count = 0;

static SnoozedReminder snoozed_reminder;

static uint64_t current_millis()
{
    return static_cast<uint64_t>(
        esp_timer_get_time() / 1000ULL
    );
}

static int minutes_from_midnight(
    int hour,
    int minute
)
{
    return (hour * 60) + minute;
}

static bool time_inside_window(
    int current_hour,
    int current_minute,
    int start_hour,
    int start_minute,
    int end_hour,
    int end_minute
)
{
    const int current =
        minutes_from_midnight(
            current_hour,
            current_minute
        );

    const int start =
        minutes_from_midnight(
            start_hour,
            start_minute
        );

    const int end =
        minutes_from_midnight(
            end_hour,
            end_minute
        );

    if (start <= end)
    {
        return current >= start && current <= end;
    }

    /*
     * Overnight window, for example 22:00 to 07:00.
     */
    return current >= start || current <= end;
}

static bool day_allowed(
    uint8_t day_mask,
    int week_day
)
{
    if (day_mask == 0)
    {
        return true;
    }

    if (week_day < 0 || week_day > 6)
    {
        return false;
    }

    return (day_mask & (1U << week_day)) != 0;
}

static int date_to_number(
    int year,
    int month,
    int day
)
{
    return (year * 10000) +
           (month * 100) +
           day;
}

static bool valid_date(
    const ReminderDate& date
)
{
    return
        date.year > 0 &&
        date.month >= 1 &&
        date.month <= 12 &&
        date.day >= 1 &&
        date.day <= 31;
}

static bool date_inside_range(
    const std::tm& time_info,
    const ReminderDate& start_date,
    const ReminderDate& end_date
)
{
    const int current =
        date_to_number(
            time_info.tm_year + 1900,
            time_info.tm_mon + 1,
            time_info.tm_mday
        );

    if (valid_date(start_date))
    {
        const int start =
            date_to_number(
                start_date.year,
                start_date.month,
                start_date.day
            );

        if (current < start)
        {
            return false;
        }
    }

    if (valid_date(end_date))
    {
        const int end =
            date_to_number(
                end_date.year,
                end_date.month,
                end_date.day
            );

        if (current > end)
        {
            return false;
        }
    }

    return true;
}

static bool same_calendar_date(
    const std::tm& time_info,
    const ReminderDate& date
)
{
    return
        valid_date(date) &&
        time_info.tm_year + 1900 == date.year &&
        time_info.tm_mon + 1 == date.month &&
        time_info.tm_mday == date.day;
}

static int standard_type_to_index(
    ReminderType type
)
{
    switch (type)
    {
        case ReminderType::HYDRATION:
            return 0;

        case ReminderType::STRETCH:
            return 1;

        case ReminderType::EYE:
            return 2;

        case ReminderType::WALK:
            return 3;

        default:
            return -1;
    }
}

static const char* reminder_type_name(
    ReminderType type
)
{
    switch (type)
    {
        case ReminderType::HYDRATION:
            return "hydration";

        case ReminderType::STRETCH:
            return "stretch";

        case ReminderType::EYE:
            return "eye";

        case ReminderType::WALK:
            return "walk";

        case ReminderType::MEDITATION:
            return "meditation";

        case ReminderType::MEDICATION:
            return "medication";

        case ReminderType::CUSTOM:
            return "custom";

        default:
            return "unknown";
    }
}

static bool queue_contains(
    ReminderType type,
    int item_index,
    int schedule_index
)
{
    if (
        active_reminder.active &&
        active_reminder.type == type &&
        active_reminder.item_index == item_index &&
        active_reminder.schedule_index == schedule_index
    )
    {
        return true;
    }

    for (std::size_t i = 0; i < queue_count; ++i)
    {
        const std::size_t index =
            (queue_head + i) % REMINDER_QUEUE_SIZE;

        const QueuedReminder& queued =
            reminder_queue[index];

        if (
            queued.type == type &&
            queued.item_index == item_index &&
            queued.schedule_index == schedule_index
        )
        {
            return true;
        }
    }

    return false;
}

static bool enqueue_reminder(
    const QueuedReminder& reminder
)
{
    if (queue_count >= REMINDER_QUEUE_SIZE)
    {
        ESP_LOGW(
            TAG,
            "Queue full, dropping %s",
            reminder_type_name(reminder.type)
        );

        return false;
    }

    if (
        queue_contains(
            reminder.type,
            reminder.item_index,
            reminder.schedule_index
        )
    )
    {
        return false;
    }

    reminder_queue[queue_tail] = reminder;

    queue_tail =
        (queue_tail + 1) %
        REMINDER_QUEUE_SIZE;

    ++queue_count;

    return true;
}

static bool dequeue_reminder(
    QueuedReminder& reminder
)
{
    if (queue_count == 0)
    {
        return false;
    }

    reminder = reminder_queue[queue_head];

    queue_head =
        (queue_head + 1) %
        REMINDER_QUEUE_SIZE;

    --queue_count;

    return true;
}

static void start_reminder(
    const QueuedReminder& reminder
)
{
    active_reminder.active = true;
    active_reminder.type = reminder.type;

    active_reminder.item_index =
        reminder.item_index;

    active_reminder.schedule_index =
        reminder.schedule_index;

    active_reminder.started_ms =
        current_millis();

    active_reminder.display_ms =
        reminder.display_ms;

    active_reminder.require_ack =
        reminder.require_ack;

    active_reminder.snooze_min =
        reminder.snooze_min;

    ESP_LOGI(
        TAG,
        "Triggered %s, item=%d, schedule=%d",
        reminder_type_name(reminder.type),
        reminder.item_index,
        reminder.schedule_index
    );

    if (trigger_callback != nullptr)
    {
        trigger_callback(
            reminder.type,
            reminder.item_index,
            reminder.schedule_index
        );
    }
}

static void finish_active_reminder()
{
    if (!active_reminder.active)
    {
        return;
    }

    const ReminderType finished_type =
        active_reminder.type;

    const int finished_item =
        active_reminder.item_index;

    const int finished_schedule =
        active_reminder.schedule_index;

    active_reminder = {};

    if (finished_callback != nullptr)
    {
        finished_callback(
            finished_type,
            finished_item,
            finished_schedule
        );
    }
}

static void start_next_queued_reminder()
{
    if (active_reminder.active)
    {
        return;
    }

    QueuedReminder reminder;

    if (dequeue_reminder(reminder))
    {
        start_reminder(reminder);
    }
}

static void update_standard_reminder(
    ReminderType type,
    const std::tm& time_info,
    uint64_t now_ms
)
{
    const int index =
        standard_type_to_index(type);

    if (index < 0)
    {
        return;
    }

    const ReminderConfig& config =
        standard_configs[index];

    StandardReminderRuntime& runtime =
        standard_runtime[index];

    if (!config.enabled)
    {
        return;
    }

    if (!day_allowed(config.day_mask, time_info.tm_wday))
    {
        return;
    }

    if (
        !time_inside_window(
            time_info.tm_hour,
            time_info.tm_min,
            config.start_hour,
            config.start_minute,
            config.end_hour,
            config.end_minute
        )
    )
    {
        return;
    }

    QueuedReminder reminder;

    reminder.type = type;
    reminder.display_ms = config.display_ms;
    reminder.require_ack = config.require_ack;
    reminder.snooze_min = config.snooze_min;

    if (config.mode == ReminderMode::INTERVAL)
    {
        if (config.interval_ms == 0)
        {
            return;
        }

        if (runtime.last_trigger_ms == 0)
        {
            runtime.last_trigger_ms = now_ms;
            return;
        }

        if (
            now_ms - runtime.last_trigger_ms >=
            config.interval_ms
        )
        {
            if (enqueue_reminder(reminder))
            {
                runtime.last_trigger_ms = now_ms;
            }
        }

        return;
    }

    const int current_minute =
        minutes_from_midnight(
            time_info.tm_hour,
            time_info.tm_min
        );

    for (
        std::size_t i = 0;
        i < config.absolute_time_count;
        ++i
    )
    {
        const ReminderTime& configured_time =
            config.absolute_times[i];

        const int configured_minute =
            minutes_from_midnight(
                configured_time.hour,
                configured_time.minute
            );

        if (current_minute != configured_minute)
        {
            continue;
        }

        const bool already_fired =
            runtime.last_absolute_year ==
                time_info.tm_year &&
            runtime.last_absolute_year_day ==
                time_info.tm_yday &&
            runtime.last_absolute_minute ==
                current_minute;

        if (already_fired)
        {
            return;
        }

        reminder.schedule_index =
            static_cast<int>(i);

        if (enqueue_reminder(reminder))
        {
            runtime.last_absolute_year =
                time_info.tm_year;

            runtime.last_absolute_year_day =
                time_info.tm_yday;

            runtime.last_absolute_minute =
                current_minute;
        }

        return;
    }
}

static void update_meditation(
    const std::tm& time_info
)
{
    if (!meditation_config.enabled)
    {
        return;
    }

    if (
        !day_allowed(
            meditation_config.day_mask,
            time_info.tm_wday
        )
    )
    {
        return;
    }

    if (
        !time_inside_window(
            time_info.tm_hour,
            time_info.tm_min,
            meditation_config.start_hour,
            meditation_config.start_minute,
            meditation_config.end_hour,
            meditation_config.end_minute
        )
    )
    {
        return;
    }

    const bool already_triggered =
        meditation_runtime.last_trigger_year ==
            time_info.tm_year &&
        meditation_runtime.last_trigger_year_day ==
            time_info.tm_yday;

    if (already_triggered)
    {
        return;
    }

    QueuedReminder reminder;

    reminder.type = ReminderType::MEDITATION;
    reminder.display_ms = meditation_config.display_ms;
    reminder.require_ack = meditation_config.require_ack;
    reminder.snooze_min = meditation_config.snooze_min;

    if (enqueue_reminder(reminder))
    {
        meditation_runtime.last_trigger_year =
            time_info.tm_year;

        meditation_runtime.last_trigger_year_day =
            time_info.tm_yday;
    }
}

static void update_medication(
    const std::tm& time_info
)
{
    if (!medication_config.enabled)
    {
        return;
    }

    const int current_minute =
        minutes_from_midnight(
            time_info.tm_hour,
            time_info.tm_min
        );

    for (
        std::size_t medicine_index = 0;
        medicine_index <
            medication_config.medicine_count;
        ++medicine_index
    )
    {
        const MedicationItem& medicine =
            medication_config.medicines[
                medicine_index
            ];

        if (!medicine.enabled)
        {
            continue;
        }

        if (
            !date_inside_range(
                time_info,
                medicine.start_date,
                medicine.end_date
            )
        )
        {
            continue;
        }

        if (
            !day_allowed(
                medicine.day_mask,
                time_info.tm_wday
            )
        )
        {
            continue;
        }

        for (
            std::size_t dose_index = 0;
            dose_index < medicine.dose_count;
            ++dose_index
        )
        {
            const ReminderTime& dose =
                medicine.doses[dose_index];

            const int dose_minute =
                minutes_from_midnight(
                    dose.hour,
                    dose.minute
                );

            if (dose_minute != current_minute)
            {
                continue;
            }

            MedicationDoseRuntime& runtime =
                medication_runtime[
                    medicine_index
                ][dose_index];

            const bool already_fired =
                runtime.last_trigger_year ==
                    time_info.tm_year &&
                runtime.last_trigger_year_day ==
                    time_info.tm_yday &&
                runtime.last_trigger_minute ==
                    current_minute;

            if (already_fired)
            {
                continue;
            }

            QueuedReminder reminder;

            reminder.type =
                ReminderType::MEDICATION;

            reminder.item_index =
                static_cast<int>(medicine_index);

            reminder.schedule_index =
                static_cast<int>(dose_index);

            reminder.display_ms =
                medication_config.display_ms;

            reminder.require_ack =
                medication_config.require_ack;

            reminder.snooze_min =
                medication_config.snooze_min;

            if (enqueue_reminder(reminder))
            {
                runtime.last_trigger_year =
                    time_info.tm_year;

                runtime.last_trigger_year_day =
                    time_info.tm_yday;

                runtime.last_trigger_minute =
                    current_minute;
            }
        }
    }
}

static void update_custom(
    const std::tm& time_info
)
{
    if (!custom_config.enabled)
    {
        return;
    }

    const int current_minute =
        minutes_from_midnight(
            time_info.tm_hour,
            time_info.tm_min
        );

    for (
        std::size_t event_index = 0;
        event_index < custom_config.event_count;
        ++event_index
    )
    {
        const CustomEvent& event =
            custom_config.events[event_index];

        if (!event.enabled)
        {
            continue;
        }

        const int event_minute =
            minutes_from_midnight(
                event.hour,
                event.minute
            );

        if (current_minute != event_minute)
        {
            continue;
        }

        bool schedule_allowed = false;

        if (
            event.type ==
            CustomEventType::RECURRING
        )
        {
            schedule_allowed =
                day_allowed(
                    event.day_mask,
                    time_info.tm_wday
                );
        }
        else
        {
            schedule_allowed =
                same_calendar_date(
                    time_info,
                    event.date
                );
        }

        if (!schedule_allowed)
        {
            continue;
        }

        CustomEventRuntime& runtime =
            custom_runtime[event_index];

        const bool already_fired =
            runtime.last_trigger_year ==
                time_info.tm_year &&
            runtime.last_trigger_year_day ==
                time_info.tm_yday &&
            runtime.last_trigger_minute ==
                current_minute;

        if (already_fired)
        {
            continue;
        }

        QueuedReminder reminder;

        reminder.type = ReminderType::CUSTOM;

        reminder.item_index =
            static_cast<int>(event_index);

        reminder.display_ms =
            event.display_ms;

        reminder.require_ack =
            custom_config.require_ack;

        reminder.snooze_min =
            custom_config.snooze_min;

        if (enqueue_reminder(reminder))
        {
            runtime.last_trigger_year =
                time_info.tm_year;

            runtime.last_trigger_year_day =
                time_info.tm_yday;

            runtime.last_trigger_minute =
                current_minute;
        }
    }
}

void reminder_engine_init()
{
    standard_configs = {};
    standard_runtime = {};

    meditation_config = {};
    meditation_runtime = {};

    medication_config = {};

    std::memset(
        medication_runtime,
        0xFF,
        sizeof(medication_runtime)
    );

    custom_config = {};

    std::memset(
        custom_runtime,
        0xFF,
        sizeof(custom_runtime)
    );

    active_reminder = {};

    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;

    snoozed_reminder = {};

    ESP_LOGI(TAG, "Reminder engine initialized");
}

void reminder_engine_set_trigger_callback(
    ReminderTriggerCallback callback
)
{
    trigger_callback = callback;
}

void reminder_engine_set_finished_callback(
    ReminderFinishedCallback callback
)
{
    finished_callback = callback;
}

bool reminder_engine_set_config(
    ReminderType type,
    const ReminderConfig& config
)
{
    const int index =
        standard_type_to_index(type);

    if (index < 0)
    {
        return false;
    }

    standard_configs[index] = config;
    standard_runtime[index] = {};

    return true;
}

const ReminderConfig*
reminder_engine_get_config(
    ReminderType type
)
{
    const int index =
        standard_type_to_index(type);

    if (index < 0)
    {
        return nullptr;
    }

    return &standard_configs[index];
}

void reminder_engine_set_meditation_config(
    const MeditationConfig& config
)
{
    meditation_config = config;
    meditation_runtime = {};
}

const MeditationConfig*
reminder_engine_get_meditation_config()
{
    return &meditation_config;
}

void reminder_engine_set_medication_config(
    const MedicationConfig& config
)
{
    medication_config = config;

    std::memset(
        medication_runtime,
        0xFF,
        sizeof(medication_runtime)
    );
}

const MedicationConfig*
reminder_engine_get_medication_config()
{
    return &medication_config;
}

void reminder_engine_set_custom_config(
    const CustomReminderConfig& config
)
{
    custom_config = config;

    std::memset(
        custom_runtime,
        0xFF,
        sizeof(custom_runtime)
    );
}

const CustomReminderConfig*
reminder_engine_get_custom_config()
{
    return &custom_config;
}

void reminder_engine_update(
    std::time_t current_time
)
{
    const uint64_t now_ms =
        current_millis();

    if (active_reminder.active)
    {
        if (
            !active_reminder.require_ack &&
            now_ms - active_reminder.started_ms >=
                active_reminder.display_ms
        )
        {
            finish_active_reminder();
        }
    }

    if (
        snoozed_reminder.valid &&
        now_ms >= snoozed_reminder.due_ms
    )
    {
        enqueue_reminder(
            snoozed_reminder.reminder
        );

        snoozed_reminder.valid = false;
    }

    std::tm time_info = {};

    localtime_r(
        &current_time,
        &time_info
    );

    update_standard_reminder(
        ReminderType::HYDRATION,
        time_info,
        now_ms
    );

    update_standard_reminder(
        ReminderType::STRETCH,
        time_info,
        now_ms
    );

    update_standard_reminder(
        ReminderType::EYE,
        time_info,
        now_ms
    );

    update_standard_reminder(
        ReminderType::WALK,
        time_info,
        now_ms
    );

    update_meditation(time_info);
    update_medication(time_info);
    update_custom(time_info);

    start_next_queued_reminder();
}

bool reminder_engine_has_active_reminder()
{
    return active_reminder.active;
}

const ActiveReminder*
reminder_engine_get_active_reminder()
{
    if (!active_reminder.active)
    {
        return nullptr;
    }

    return &active_reminder;
}

ReminderType reminder_engine_get_active_type()
{
    return active_reminder.type;
}

void reminder_engine_acknowledge_active()
{
    finish_active_reminder();
    start_next_queued_reminder();
}

void reminder_engine_snooze_active()
{
    if (!active_reminder.active)
    {
        return;
    }

    if (active_reminder.snooze_min == 0)
    {
        reminder_engine_acknowledge_active();
        return;
    }

    QueuedReminder reminder;

    reminder.type =
        active_reminder.type;

    reminder.item_index =
        active_reminder.item_index;

    reminder.schedule_index =
        active_reminder.schedule_index;

    reminder.display_ms =
        active_reminder.display_ms;

    reminder.require_ack =
        active_reminder.require_ack;

    reminder.snooze_min =
        active_reminder.snooze_min;

    snoozed_reminder.valid = true;
    snoozed_reminder.reminder = reminder;

    snoozed_reminder.due_ms =
        current_millis() +
        (
            static_cast<uint64_t>(
                active_reminder.snooze_min
            ) *
            60ULL *
            1000ULL
        );

    finish_active_reminder();
    start_next_queued_reminder();
}

void reminder_engine_cancel_active()
{
    finish_active_reminder();
    start_next_queued_reminder();
}