#include "user_statistics.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "nvs.h"
#include "consumption_tracker.hpp"
#include "statistics_history.hpp"

namespace
{
constexpr const char* TAG = "USER_STATS";
constexpr const char* NVS_NAMESPACE = "statistics";
constexpr const char* NVS_KEY = "today_v1";
constexpr uint32_t STORED_VERSION = 1;

struct BasicCounter
{
    uint32_t acknowledged = 0;
    uint32_t missed = 0;
};

struct TokenCounter
{
    bool used = false;
    char token_id[REMINDER_ID_LENGTH] = {};
    uint32_t acknowledged = 0;
    uint32_t missed = 0;
    uint32_t snoozed = 0;
    int64_t last_ack_timestamp = 0;
    int64_t last_miss_timestamp = 0;
};

struct StoredStatistics
{
    uint32_t version = STORED_VERSION;
    uint32_t yyyymmdd = 0;

    uint32_t hydration_ml = 0;
    BasicCounter hydration = {};
    BasicCounter stretch = {};
    BasicCounter eye = {};
    BasicCounter walk = {};
    BasicCounter meditation = {};

    TokenCounter medicines[MAX_MEDICINES] = {};
    TokenCounter custom_events[MAX_CUSTOM_EVENTS] = {};
};

StoredStatistics statistics_data = {};
bool initialized = false;

uint32_t current_yyyymmdd()
{
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_r(&now, &local);

    const int year = local.tm_year + 1900;
    if (year < 2025)
    {
        return 0;
    }

    return static_cast<uint32_t>(
        year * 10000 +
        (local.tm_mon + 1) * 100 +
        local.tm_mday
    );
}

void clear_for_day(uint32_t day)
{
    statistics_data = {};
    statistics_data.version = STORED_VERSION;
    statistics_data.yyyymmdd = day;
}

void save()
{
    if (!initialized)
    {
        return;
    }

    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(error));
        return;
    }

    error = nvs_set_blob(handle, NVS_KEY, &statistics_data, sizeof(statistics_data));
    if (error == ESP_OK)
    {
        error = nvs_commit(handle);
    }
    nvs_close(handle);

    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "statistics save failed: %s", esp_err_to_name(error));
    }
}

DailyStatisticsRecord make_history_record(const StoredStatistics& source)
{
    DailyStatisticsRecord record = {};
    record.version = 1;
    record.yyyymmdd = source.yyyymmdd;
    record.hydration_goal_ml = consumption_tracker_daily_goal_ml();
    record.hydration_ml = source.hydration_ml;

    record.hydration.acknowledged = source.hydration.acknowledged;
    record.hydration.missed = source.hydration.missed;
    record.stretch.acknowledged = source.stretch.acknowledged;
    record.stretch.missed = source.stretch.missed;
    record.eye.acknowledged = source.eye.acknowledged;
    record.eye.missed = source.eye.missed;
    record.walk.acknowledged = source.walk.acknowledged;
    record.walk.missed = source.walk.missed;
    record.meditation.acknowledged = source.meditation.acknowledged;
    record.meditation.missed = source.meditation.missed;

    for (std::size_t i = 0; i < MAX_MEDICINES; ++i)
    {
        const TokenCounter& input = source.medicines[i];
        HistoryTokenCounter& output = record.medicines[i];
        output.used = input.used;
        std::strncpy(output.token_id, input.token_id, REMINDER_ID_LENGTH - 1);
        output.acknowledged = input.acknowledged;
        output.missed = input.missed;
        output.snoozed = input.snoozed;
        output.last_ack_timestamp = input.last_ack_timestamp;
        output.last_miss_timestamp = input.last_miss_timestamp;
    }

    for (std::size_t i = 0; i < MAX_CUSTOM_EVENTS; ++i)
    {
        const TokenCounter& input = source.custom_events[i];
        HistoryTokenCounter& output = record.custom_events[i];
        output.used = input.used;
        std::strncpy(output.token_id, input.token_id, REMINDER_ID_LENGTH - 1);
        output.acknowledged = input.acknowledged;
        output.missed = input.missed;
        output.snoozed = input.snoozed;
        output.last_ack_timestamp = input.last_ack_timestamp;
        output.last_miss_timestamp = input.last_miss_timestamp;
    }

    return record;
}

void archive_completed_day_if_valid(const StoredStatistics& completed)
{
    if (completed.yyyymmdd < 20250101)
    {
        return;
    }

    if (!statistics_history_is_initialized())
    {
        ESP_LOGW(TAG, "History unavailable; completed day %lu not archived",
                 static_cast<unsigned long>(completed.yyyymmdd));
        return;
    }

    const DailyStatisticsRecord record = make_history_record(completed);
    if (statistics_history_append(record))
    {
        ESP_LOGI(TAG, "Archived statistics day %lu",
                 static_cast<unsigned long>(completed.yyyymmdd));
    }
    else
    {
        ESP_LOGE(TAG, "Failed to archive statistics day %lu",
                 static_cast<unsigned long>(completed.yyyymmdd));
    }
}

void ensure_current_day()
{
    const uint32_t today = current_yyyymmdd();
    if (today == 0)
    {
        return;
    }

    if (statistics_data.yyyymmdd != today)
    {
        const StoredStatistics completed = statistics_data;
        archive_completed_day_if_valid(completed);
        clear_for_day(today);
        save();
        ESP_LOGI(TAG, "Started statistics day %lu", static_cast<unsigned long>(today));
    }
}

BasicCounter* basic_counter(ReminderType type)
{
    switch (type)
    {
        case ReminderType::HYDRATION: return &statistics_data.hydration;
        case ReminderType::STRETCH: return &statistics_data.stretch;
        case ReminderType::EYE: return &statistics_data.eye;
        case ReminderType::WALK: return &statistics_data.walk;
        case ReminderType::MEDITATION: return &statistics_data.meditation;
        default: return nullptr;
    }
}

template <std::size_t N>
TokenCounter* find_or_create(TokenCounter (&records)[N], const char* token_id)
{
    if (token_id == nullptr || token_id[0] == '\0')
    {
        return nullptr;
    }

    for (TokenCounter& record : records)
    {
        if (record.used && std::strncmp(record.token_id, token_id, REMINDER_ID_LENGTH) == 0)
        {
            return &record;
        }
    }

    for (TokenCounter& record : records)
    {
        if (!record.used)
        {
            record = {};
            record.used = true;
            std::strncpy(record.token_id, token_id, REMINDER_ID_LENGTH - 1);
            record.token_id[REMINDER_ID_LENGTH - 1] = '\0';
            return &record;
        }
    }

    ESP_LOGW(TAG, "No free token statistics slot for %s", token_id);
    return nullptr;
}

std::size_t used_medication_count()
{
    std::size_t count = 0;
    for (const TokenCounter& record : statistics_data.medicines)
    {
        count += record.used ? 1U : 0U;
    }
    return count;
}

std::size_t used_custom_count()
{
    std::size_t count = 0;
    for (const TokenCounter& record : statistics_data.custom_events)
    {
        count += record.used ? 1U : 0U;
    }
    return count;
}

const TokenCounter* nth_used_token(
    const TokenCounter* records,
    std::size_t record_count,
    std::size_t wanted
)
{
    std::size_t current = 0;
    for (std::size_t i = 0; i < record_count; ++i)
    {
        if (!records[i].used)
        {
            continue;
        }
        if (current == wanted)
        {
            return &records[i];
        }
        ++current;
    }
    return nullptr;
}
}

esp_err_t user_statistics_init()
{
    clear_for_day(current_yyyymmdd());

    nvs_handle_t handle = 0;
    const esp_err_t open_error = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (open_error == ESP_OK)
    {
        StoredStatistics stored = {};
        std::size_t size = sizeof(stored);
        const esp_err_t read_error = nvs_get_blob(handle, NVS_KEY, &stored, &size);
        nvs_close(handle);

        if (
            read_error == ESP_OK &&
            size == sizeof(stored) &&
            stored.version == STORED_VERSION
        )
        {
            /* Load even an older day. ensure_current_day() archives it first. */
            statistics_data = stored;
        }
    }

    initialized = true;
    ensure_current_day();
    ESP_LOGI(TAG, "Statistics initialized for %lu", static_cast<unsigned long>(statistics_data.yyyymmdd));
    return ESP_OK;
}

void user_statistics_update_day()
{
    if (initialized)
    {
        ensure_current_day();
    }
}

void user_statistics_record_ack(ReminderType type, const char* token_id)
{
    if (!initialized) return;
    ensure_current_day();

    if (BasicCounter* counter = basic_counter(type))
    {
        ++counter->acknowledged;
    }
    else if (type == ReminderType::MEDICATION)
    {
        if (TokenCounter* record = find_or_create(statistics_data.medicines, token_id))
        {
            ++record->acknowledged;
            record->last_ack_timestamp = static_cast<int64_t>(std::time(nullptr));
        }
    }
    else if (type == ReminderType::CUSTOM)
    {
        if (TokenCounter* record = find_or_create(statistics_data.custom_events, token_id))
        {
            ++record->acknowledged;
            record->last_ack_timestamp = static_cast<int64_t>(std::time(nullptr));
        }
    }

    save();
}

void user_statistics_record_miss(ReminderType type, const char* token_id)
{
    if (!initialized) return;
    ensure_current_day();

    if (BasicCounter* counter = basic_counter(type))
    {
        ++counter->missed;
    }
    else if (type == ReminderType::MEDICATION)
    {
        if (TokenCounter* record = find_or_create(statistics_data.medicines, token_id))
        {
            ++record->missed;
            record->last_miss_timestamp = static_cast<int64_t>(std::time(nullptr));
        }
    }
    else if (type == ReminderType::CUSTOM)
    {
        if (TokenCounter* record = find_or_create(statistics_data.custom_events, token_id))
        {
            ++record->missed;
            record->last_miss_timestamp = static_cast<int64_t>(std::time(nullptr));
        }
    }

    save();
}

void user_statistics_record_medication_snooze(const char* token_id)
{
    if (!initialized) return;
    ensure_current_day();

    if (TokenCounter* record = find_or_create(statistics_data.medicines, token_id))
    {
        ++record->snoozed;
        save();
    }
}

void user_statistics_add_hydration_ml(uint32_t amount_ml)
{
    if (!initialized || amount_ml == 0) return;
    ensure_current_day();
    statistics_data.hydration_ml += amount_ml;
    save();
}

void user_statistics_reset_today()
{
    if (!initialized) return;
    clear_for_day(current_yyyymmdd());
    save();
}

std::size_t user_statistics_line_count()
{
    if (!initialized) return 0;
    ensure_current_day();
    return 8 + used_medication_count() + used_custom_count();
}

bool user_statistics_get_line(std::size_t index, char* output, std::size_t output_size)
{
    if (!initialized || output == nullptr || output_size == 0)
    {
        return false;
    }

    ensure_current_day();
    int written = -1;

    if (index == 0)
    {
        written = std::snprintf(output, output_size, "STATS_BEGIN:v1");
    }
    else if (index == 1)
    {
        written = std::snprintf(output, output_size, "DAY:%lu", static_cast<unsigned long>(statistics_data.yyyymmdd));
    }
    else if (index == 2)
    {
        written = std::snprintf(output, output_size, "HYD:ML=%lu,ACK=%lu,MISS=%lu",
            static_cast<unsigned long>(statistics_data.hydration_ml),
            static_cast<unsigned long>(statistics_data.hydration.acknowledged),
            static_cast<unsigned long>(statistics_data.hydration.missed));
    }
    else if (index == 3)
    {
        written = std::snprintf(output, output_size, "STR:ACK=%lu,MISS=%lu",
            static_cast<unsigned long>(statistics_data.stretch.acknowledged),
            static_cast<unsigned long>(statistics_data.stretch.missed));
    }
    else if (index == 4)
    {
        written = std::snprintf(output, output_size, "EYE:ACK=%lu,MISS=%lu",
            static_cast<unsigned long>(statistics_data.eye.acknowledged),
            static_cast<unsigned long>(statistics_data.eye.missed));
    }
    else if (index == 5)
    {
        written = std::snprintf(output, output_size, "WALK:ACK=%lu,MISS=%lu",
            static_cast<unsigned long>(statistics_data.walk.acknowledged),
            static_cast<unsigned long>(statistics_data.walk.missed));
    }
    else if (index == 6)
    {
        written = std::snprintf(output, output_size, "MEDIT:ACK=%lu,MISS=%lu",
            static_cast<unsigned long>(statistics_data.meditation.acknowledged),
            static_cast<unsigned long>(statistics_data.meditation.missed));
    }
    else
    {
        const std::size_t medication_count = used_medication_count();
        const std::size_t medication_start = 7;
        const std::size_t custom_start = medication_start + medication_count;
        const std::size_t end_index = custom_start + used_custom_count();

        if (index < custom_start)
        {
            const TokenCounter* record = nth_used_token(
                statistics_data.medicines,
                MAX_MEDICINES,
                index - medication_start
            );
            if (record != nullptr)
            {
                written = std::snprintf(output, output_size,
                    "MED:%s,A=%lu,M=%lu,S=%lu,LA=%lld,LM=%lld",
                    record->token_id,
                    static_cast<unsigned long>(record->acknowledged),
                    static_cast<unsigned long>(record->missed),
                    static_cast<unsigned long>(record->snoozed),
                    static_cast<long long>(record->last_ack_timestamp),
                    static_cast<long long>(record->last_miss_timestamp));
            }
        }
        else if (index < end_index)
        {
            const TokenCounter* record = nth_used_token(
                statistics_data.custom_events,
                MAX_CUSTOM_EVENTS,
                index - custom_start
            );
            if (record != nullptr)
            {
                written = std::snprintf(output, output_size,
                    "CUSTOM:%s,A=%lu,M=%lu,LA=%lld,LM=%lld",
                    record->token_id,
                    static_cast<unsigned long>(record->acknowledged),
                    static_cast<unsigned long>(record->missed),
                    static_cast<long long>(record->last_ack_timestamp),
                    static_cast<long long>(record->last_miss_timestamp));
            }
        }
        else if (index == end_index)
        {
            written = std::snprintf(output, output_size, "STATS_END");
        }
    }

    return written >= 0 && static_cast<std::size_t>(written) < output_size;
}