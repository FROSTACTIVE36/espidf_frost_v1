#pragma once

#include <cstddef>
#include <cstdint>

static constexpr std::size_t MAX_ABSOLUTE_TIMES = 16;

static constexpr std::size_t MAX_MEDICINES = 8;
static constexpr std::size_t MAX_DOSES_PER_MEDICINE = 8;

static constexpr std::size_t MAX_CUSTOM_EVENTS = 16;

static constexpr std::size_t REMINDER_ID_LENGTH = 24;
static constexpr std::size_t REMINDER_LABEL_LENGTH = 64;

enum class ReminderType : uint8_t
{
    HYDRATION,
    STRETCH,
    EYE,
    WALK,
    MEDITATION,
    MEDICATION,
    CUSTOM,
    COUNT
};

enum class ReminderMode : uint8_t
{
    INTERVAL,
    ABSOLUTE
};

enum class CustomEventType : uint8_t
{
    RECURRING,
    ABSOLUTE
};

struct ReminderTime
{
    uint8_t hour = 0;
    uint8_t minute = 0;
};

struct ReminderDate
{
    int year = 0;
    uint8_t month = 0;
    uint8_t day = 0;
};

/*
 * Hydration, stretch, eye and walk.
 */
struct ReminderConfig
{
    bool enabled = false;

    ReminderMode mode = ReminderMode::INTERVAL;

    uint32_t interval_ms = 0;
    uint32_t display_ms = 10000;

    bool require_ack = false;

    uint8_t start_hour = 0;
    uint8_t start_minute = 0;

    uint8_t end_hour = 23;
    uint8_t end_minute = 59;

    /*
     * Bit 0 = Sunday
     * Bit 1 = Monday
     * ...
     * Bit 6 = Saturday
     *
     * 0 means every day.
     */
    uint8_t day_mask = 0;

    ReminderTime absolute_times[MAX_ABSOLUTE_TIMES] = {};
    std::size_t absolute_time_count = 0;
};

/*
 * Meditation has no interval or absolute mode.
 * It triggers once when the device enters its daily time window.
 */
struct MeditationConfig
{
    bool enabled = false;

    uint8_t start_hour = 6;
    uint8_t start_minute = 0;

    uint8_t end_hour = 7;
    uint8_t end_minute = 0;

    uint32_t display_ms = 600000;

    bool require_ack = false;

    uint8_t day_mask = 0;
};

struct MedicationItem
{
    bool enabled = true;

    char id[REMINDER_ID_LENGTH] = {};
    char label[REMINDER_LABEL_LENGTH] = {};

    ReminderDate start_date = {};
    ReminderDate end_date = {};

    uint8_t day_mask = 0;

    ReminderTime doses[MAX_DOSES_PER_MEDICINE] = {};
    std::size_t dose_count = 0;

    /*
     * Label drawing configuration.
     */
    int16_t text_x = 120;
    int16_t text_y = 160;

    uint8_t text_size = 1;
    uint16_t text_color = 0xFFFF;

    /*
     * Text alignment:
     * 0 = left
     * 1 = center
     * 2 = right
     */
    uint8_t text_align = 1;

    /*
     * Maximum width available for wrapped text.
     */
    uint16_t text_width = 180;
};

struct MedicationConfig
{
    bool enabled = false;

    bool require_ack = true;

    /*
     * Snooze exists only for medication.
     */
    uint16_t snooze_min = 10;

    uint32_t display_ms = 60000;

    MedicationItem medicines[MAX_MEDICINES] = {};
    std::size_t medicine_count = 0;
};

struct CustomEvent
{
    bool enabled = true;

    char id[REMINDER_ID_LENGTH] = {};
    char label[REMINDER_LABEL_LENGTH] = {};

    CustomEventType type = CustomEventType::RECURRING;

    uint8_t hour = 0;
    uint8_t minute = 0;

    uint32_t display_ms = 60000;

    uint8_t day_mask = 0;
    ReminderDate date = {};

    /*
     * Label drawing configuration.
     */
    int16_t text_x = 120;
    int16_t text_y = 160;

    uint8_t text_size = 1;
    uint16_t text_color = 0xFFFF;

    /*
     * 0 = left
     * 1 = center
     * 2 = right
     */
    uint8_t text_align = 1;

    uint16_t text_width = 180;
};

struct CustomReminderConfig
{
    bool enabled = false;

    bool require_ack = true;

    CustomEvent events[MAX_CUSTOM_EVENTS] = {};
    std::size_t event_count = 0;
};

struct ActiveReminder
{
    bool active = false;

    ReminderType type = ReminderType::HYDRATION;

    /*
     * Medication:
     * item_index = medicine index
     * schedule_index = dose index
     *
     * Custom:
     * item_index = event index
     */
    int item_index = -1;
    int schedule_index = -1;

    uint64_t started_ms = 0;
    uint32_t display_ms = 0;

    bool require_ack = false;

    /*
     * Kept here because the currently active medication reminder
     * needs to store its snooze duration.
     * It remains 0 for every non-medication reminder.
     */
    uint16_t snooze_min = 0;
};