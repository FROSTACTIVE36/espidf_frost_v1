#include "reminder_engine.hpp"

#include <array>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "user_statistics.hpp"

static const char* TAG = "REMINDER_ENGINE";

static constexpr std::size_t STANDARD_REMINDER_COUNT = 4;
static constexpr std::size_t REMINDER_QUEUE_SIZE = 16;
static constexpr uint32_t ACK_QUEUE_PREVIEW_MS = 5000;

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

struct BottleCleanRuntime
{
    bool anchor_initialized = false;
    int64_t anchor_day = 0;
    int last_trigger_year = -1;
    int last_trigger_year_day = -1;
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

static BottleCleanConfig bottle_clean_config;
static BottleCleanRuntime bottle_clean_runtime;

static CustomEventRuntime
custom_runtime[MAX_CUSTOM_EVENTS];

static ActiveReminder active_reminder;

static ReminderTriggerCallback trigger_callback = nullptr;
static ReminderFinishedCallback finished_callback = nullptr;

static QueuedReminder reminder_queue[REMINDER_QUEUE_SIZE];

static std::size_t queue_head = 0;
static std::size_t queue_tail = 0;
static std::size_t queue_count = 0;

/*
 * After one acknowledgement, only the reminders that were already
 * waiting in the queue are previewed. Each is shown for five seconds
 * and then cleared automatically without requiring another ACK.
 */
static std::size_t ack_preview_remaining = 0;
static bool ack_preview_active = false;

static SnoozedReminder snoozed_reminder;

/*
 * When true, only medication reminders are permitted to become active.
 * All reminder schedules are still evaluated and non-medication reminders
 * remain queued until normal activation is restored.
 */
static bool medication_only_activation = false;
static bool dnd_enabled = false;

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

        case ReminderType::BOTTLE_CLEAN:
            return "bottle_clean";

        default:
            return "unknown";
    }
}

static const char* statistics_token_id(
    ReminderType type,
    int item_index
)
{
    if (item_index < 0)
    {
        return nullptr;
    }

    if (type == ReminderType::MEDICATION)
    {
        if (
            static_cast<std::size_t>(item_index) <
            medication_config.medicine_count
        )
        {
            return medication_config.medicines[item_index].id;
        }
    }
    else if (type == ReminderType::CUSTOM)
    {
        if (
            static_cast<std::size_t>(item_index) <
            custom_config.event_count
        )
        {
            return custom_config.events[item_index].id;
        }
    }

    return nullptr;
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
    if (dnd_enabled && reminder.type != ReminderType::MEDICATION)
    {
        /*
         * DND suppression counts as a consumed schedule event.
         * This prevents repeated retries and prevents the reminder from
         * firing later after DND is disabled.
         */
        ESP_LOGI(
            TAG,
            "DND skipped %s",
            reminder_type_name(reminder.type)
        );
        return true;
    }

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

/*
 * Removes the first queued reminder of the requested type while preserving
 * the relative order of every other queued reminder.
 */
static bool dequeue_first_reminder_of_type(
    ReminderType requested_type,
    QueuedReminder& reminder
)
{
    if (queue_count == 0)
    {
        return false;
    }

    std::size_t match_offset = queue_count;

    for (std::size_t offset = 0; offset < queue_count; ++offset)
    {
        const std::size_t index =
            (queue_head + offset) % REMINDER_QUEUE_SIZE;

        if (reminder_queue[index].type == requested_type)
        {
            match_offset = offset;
            break;
        }
    }

    if (match_offset == queue_count)
    {
        return false;
    }

    const std::size_t match_index =
        (queue_head + match_offset) % REMINDER_QUEUE_SIZE;

    reminder = reminder_queue[match_index];

    /*
     * Shift later logical queue entries one position toward the head.
     */
    for (
        std::size_t offset = match_offset;
        offset + 1 < queue_count;
        ++offset
    )
    {
        const std::size_t destination =
            (queue_head + offset) % REMINDER_QUEUE_SIZE;

        const std::size_t source =
            (queue_head + offset + 1) % REMINDER_QUEUE_SIZE;

        reminder_queue[destination] =
            reminder_queue[source];
    }

    queue_tail =
        (queue_tail + REMINDER_QUEUE_SIZE - 1) %
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

    /*
     * Priority 1:
     * Always select the first queued medication reminder before any
     * hydration, stretch, eye, walk, meditation, or custom reminder.
     *
     * This preserves the relative order of all non-medication reminders.
     */
    bool reminder_available =
        dequeue_first_reminder_of_type(
            ReminderType::MEDICATION,
            reminder
        );

    if (!reminder_available)
    {
        /*
         * During Pomodoro focus, only medication reminders may become
         * active. Non-medication reminders remain queued until focus ends.
         */
        if (medication_only_activation)
        {
            return;
        }

        /*
         * No medication is waiting, so continue with normal FIFO order.
         */
        reminder_available =
            dequeue_reminder(reminder);
    }

    if (!reminder_available)
    {
        ack_preview_active = false;
        ack_preview_remaining = 0;
        return;
    }

    /*
     * During the acknowledge-all sequence, queued reminders are only
     * previews. They are displayed for five seconds and do not wait for
     * another acknowledgement.
     */
    if (ack_preview_active && ack_preview_remaining > 0)
    {
        reminder.display_ms = ACK_QUEUE_PREVIEW_MS;
        reminder.require_ack = false;
        reminder.snooze_min = 0;

        --ack_preview_remaining;

        ESP_LOGI(
            TAG,
            "ACK queue preview: %s, remaining after this=%u",
            reminder_type_name(reminder.type),
            static_cast<unsigned>(ack_preview_remaining)
        );
    }

    start_reminder(reminder);

    /*
     * The current preview is the final reminder from the queue snapshot.
     * Clear preview mode now; the active reminder still runs for five
     * seconds because its copied ActiveReminder values are already set.
     * Any reminders queued later retain their normal behaviour.
     */
    if (ack_preview_active && ack_preview_remaining == 0)
    {
        ack_preview_active = false;
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

static int64_t calendar_day_number(const std::tm& time_info)
{
    std::tm midnight = time_info;
    midnight.tm_hour = 0;
    midnight.tm_min = 0;
    midnight.tm_sec = 0;
    midnight.tm_isdst = -1;

    const std::time_t value = std::mktime(&midnight);
    if (value < 0)
    {
        return 0;
    }

    return static_cast<int64_t>(value / 86400);
}

static void update_bottle_clean(const std::tm& time_info)
{
    if (!bottle_clean_config.enabled || bottle_clean_config.interval_days == 0)
    {
        return;
    }

    const int64_t today = calendar_day_number(time_info);

    if (!bottle_clean_runtime.anchor_initialized)
    {
        bottle_clean_runtime.anchor_initialized = true;
        bottle_clean_runtime.anchor_day = today;
        return;
    }

    if (
        time_info.tm_hour != bottle_clean_config.hour ||
        time_info.tm_min != bottle_clean_config.minute
    )
    {
        return;
    }

    const bool already_fired =
        bottle_clean_runtime.last_trigger_year == time_info.tm_year &&
        bottle_clean_runtime.last_trigger_year_day == time_info.tm_yday;

    if (already_fired)
    {
        return;
    }

    const int64_t elapsed_days = today - bottle_clean_runtime.anchor_day;
    if (elapsed_days < static_cast<int64_t>(bottle_clean_config.interval_days))
    {
        return;
    }

    QueuedReminder reminder;
    reminder.type = ReminderType::BOTTLE_CLEAN;
    reminder.display_ms = bottle_clean_config.display_ms;
    reminder.require_ack = bottle_clean_config.require_ack;

    if (enqueue_reminder(reminder))
    {
        bottle_clean_runtime.last_trigger_year = time_info.tm_year;
        bottle_clean_runtime.last_trigger_year_day = time_info.tm_yday;
        bottle_clean_runtime.anchor_day = today;
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

    bottle_clean_config = {};
    bottle_clean_runtime = {};

    std::memset(
        custom_runtime,
        0xFF,
        sizeof(custom_runtime)
    );

    active_reminder = {};

    queue_head = 0;
    queue_tail = 0;
    queue_count = 0;

    ack_preview_remaining = 0;
    ack_preview_active = false;

    snoozed_reminder = {};

    medication_only_activation = false;
    dnd_enabled = false;

    user_statistics_init();

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

void reminder_engine_set_bottle_clean_config(
    const BottleCleanConfig& config
)
{
    bottle_clean_config = config;
    bottle_clean_runtime = {};
}

const BottleCleanConfig*
reminder_engine_get_bottle_clean_config()
{
    return &bottle_clean_config;
}

void reminder_engine_set_medication_only_activation(
    bool enabled
)
{
    if (medication_only_activation == enabled)
    {
        return;
    }

    medication_only_activation = enabled;

    ESP_LOGI(
        TAG,
        "Medication-only activation %s",
        enabled ? "enabled" : "disabled"
    );

    /*
     * When focus ends, immediately allow the oldest queued reminder
     * to start instead of waiting for another scheduling cycle.
     */
    if (!medication_only_activation)
    {
        start_next_queued_reminder();
    }
}

void reminder_engine_set_dnd(bool enabled)
{
    if (dnd_enabled == enabled)
    {
        return;
    }

    dnd_enabled = enabled;

    if (dnd_enabled)
    {
        /*
         * DND means skip, not postpone.
         * Keep only medication reminders that were already queued.
         */
        QueuedReminder retained[REMINDER_QUEUE_SIZE] = {};
        std::size_t retained_count = 0;

        for (std::size_t i = 0; i < queue_count; ++i)
        {
            const std::size_t index =
                (queue_head + i) % REMINDER_QUEUE_SIZE;

            if (reminder_queue[index].type == ReminderType::MEDICATION)
            {
                retained[retained_count++] =
                    reminder_queue[index];
            }
        }

        queue_head = 0;
        queue_tail = 0;
        queue_count = 0;

        for (std::size_t i = 0; i < retained_count; ++i)
        {
            reminder_queue[queue_tail] = retained[i];

            queue_tail =
                (queue_tail + 1) %
                REMINDER_QUEUE_SIZE;

            ++queue_count;
        }

        ack_preview_active = false;
        ack_preview_remaining = 0;
    }

    ESP_LOGI(
        TAG,
        "DND %s",
        enabled ? "enabled" : "disabled"
    );
}

bool reminder_engine_is_dnd_enabled()
{
    return dnd_enabled;
}

void reminder_engine_update(
    std::time_t current_time
)
{
    const uint64_t now_ms =
        current_millis();

    user_statistics_update_day();

    if (active_reminder.active)
    {
        const bool timed_out =
            active_reminder.display_ms > 0 &&
            now_ms - active_reminder.started_ms >=
                active_reminder.display_ms;

        if (timed_out)
        {
            if (active_reminder.require_ack)
            {
                user_statistics_record_miss(
                    active_reminder.type,
                    statistics_token_id(
                        active_reminder.type,
                        active_reminder.item_index
                    )
                );
            }

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
    update_bottle_clean(time_info);

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
    if (!active_reminder.active)
    {
        ESP_LOGW(TAG, "No active reminder to acknowledge");
        return;
    }

    user_statistics_record_ack(
        active_reminder.type,
        statistics_token_id(
            active_reminder.type,
            active_reminder.item_index
        )
    );

    /*
     * Snapshot the queue before finishing the current reminder.
     * One ACK clears the current reminder normally, then every reminder
     * already waiting is shown for five seconds and auto-cleared.
     */
    ack_preview_remaining = queue_count;
    ack_preview_active = ack_preview_remaining > 0;

    ESP_LOGI(
        TAG,
        "Acknowledging active reminder; queued previews=%u",
        static_cast<unsigned>(ack_preview_remaining)
    );

    finish_active_reminder();
    start_next_queued_reminder();
}


std::size_t reminder_engine_get_queue_count()
{
    return queue_count;
}

bool reminder_engine_is_ack_preview_active()
{
    return ack_preview_active ||
           (
               active_reminder.active &&
               !active_reminder.require_ack &&
               active_reminder.display_ms == ACK_QUEUE_PREVIEW_MS
           );
}

void reminder_engine_snooze_active()
{
    if (!active_reminder.active)
    {
        ESP_LOGW(TAG, "No active reminder to snooze");
        return;
    }

    /*
     * Snooze is supported only for medication reminders.
     * Every other reminder ignores the snooze command.
     */
    if (active_reminder.type != ReminderType::MEDICATION)
    {
        ESP_LOGW(
            TAG,
            "Snooze rejected for %s; only medication supports snooze",
            reminder_type_name(active_reminder.type)
        );
        return;
    }

    if (active_reminder.snooze_min == 0)
    {
        ESP_LOGW(TAG, "Medication snooze duration is zero");
        return;
    }

    user_statistics_record_medication_snooze(
        statistics_token_id(
            active_reminder.type,
            active_reminder.item_index
        )
    );

    QueuedReminder reminder;

    reminder.type = ReminderType::MEDICATION;
    reminder.item_index = active_reminder.item_index;
    reminder.schedule_index = active_reminder.schedule_index;
    reminder.display_ms = active_reminder.display_ms;
    reminder.require_ack = active_reminder.require_ack;
    reminder.snooze_min = active_reminder.snooze_min;

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

    ESP_LOGI(
        TAG,
        "Medication snoozed for %u minutes",
        static_cast<unsigned>(active_reminder.snooze_min)
    );

    finish_active_reminder();
    start_next_queued_reminder();
}

void reminder_engine_cancel_active()
{
    finish_active_reminder();
    start_next_queued_reminder();
}