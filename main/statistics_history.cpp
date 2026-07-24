#include "statistics_history.hpp"

#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_spiffs.h"

namespace
{
constexpr const char* TAG = "STATS_HISTORY";
constexpr const char* BASE_PATH = "/spiffs";
constexpr const char* FILE_PATH = "/spiffs/history.bin";
constexpr uint32_t FILE_MAGIC = 0x46535448; // FSTH
constexpr uint32_t FILE_VERSION = 1;

struct HistoryFile
{
    uint32_t magic = FILE_MAGIC;
    uint32_t version = FILE_VERSION;
    uint32_t write_index = 0;
    uint32_t count = 0;
    DailyStatisticsRecord records[STATISTICS_HISTORY_DAYS] = {};
};

HistoryFile history = {};
bool initialized = false;
int selected_physical_index = -1;

uint32_t calculate_checksum(const DailyStatisticsRecord& input)
{
    DailyStatisticsRecord copy = input;
    copy.checksum = 0;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&copy);
    uint32_t hash = 2166136261u;
    for (std::size_t i = 0; i < sizeof(copy); ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

bool valid_record(const DailyStatisticsRecord& record)
{
    return record.version == 1 &&
           record.yyyymmdd >= 20250101 &&
           record.checksum == calculate_checksum(record);
}

bool save_file()
{
    FILE* file = std::fopen(FILE_PATH, "wb");
    if (file == nullptr)
    {
        ESP_LOGE(TAG, "Unable to open history file for writing");
        return false;
    }
    const bool ok = std::fwrite(&history, 1, sizeof(history), file) == sizeof(history);
    std::fflush(file);
    std::fclose(file);
    if (!ok) ESP_LOGE(TAG, "History file write failed");
    return ok;
}

void reset_memory()
{
    history = {};
    history.magic = FILE_MAGIC;
    history.version = FILE_VERSION;
    selected_physical_index = -1;
}


bool load_file()
{
    FILE* file = std::fopen(FILE_PATH, "rb");

    if (file == nullptr)
    {
        ESP_LOGW(TAG, "History file does not exist yet");
        return false;
    }

    reset_memory();

    const std::size_t bytes_read = std::fread(
        &history,
        1,
        sizeof(history),
        file
    );

    std::fclose(file);

    if (bytes_read != sizeof(history))
    {
        ESP_LOGW(
            TAG,
            "Invalid history file size: read %u, expected %u",
            static_cast<unsigned>(bytes_read),
            static_cast<unsigned>(sizeof(history))
        );

        reset_memory();
        return false;
    }

    if (
        history.magic != FILE_MAGIC ||
        history.version != FILE_VERSION ||
        history.count > STATISTICS_HISTORY_DAYS ||
        history.write_index >= STATISTICS_HISTORY_DAYS
    )
    {
        ESP_LOGW(
            TAG,
            "History file header is invalid"
        );

        reset_memory();
        return false;
    }

    ESP_LOGI(
        TAG,
        "Loaded %lu history records",
        static_cast<unsigned long>(history.count)
    );

    return true;
}

int newest_physical_index(std::size_t newest_offset)
{
    if (newest_offset >= history.count) return -1;
    const std::size_t newest = (history.write_index + STATISTICS_HISTORY_DAYS - 1) % STATISTICS_HISTORY_DAYS;
    return static_cast<int>((newest + STATISTICS_HISTORY_DAYS - newest_offset) % STATISTICS_HISTORY_DAYS);
}

std::size_t used_count(const HistoryTokenCounter* records, std::size_t count)
{
    std::size_t used = 0;
    for (std::size_t i = 0; i < count; ++i) used += records[i].used ? 1U : 0U;
    return used;
}

const HistoryTokenCounter* nth_used(const HistoryTokenCounter* records, std::size_t count, std::size_t wanted)
{
    std::size_t seen = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!records[i].used) continue;
        if (seen++ == wanted) return &records[i];
    }
    return nullptr;
}

std::size_t record_line_count(const DailyStatisticsRecord& record)
{
    return 7 + used_count(record.medicines, MAX_MEDICINES) +
           used_count(record.custom_events, MAX_CUSTOM_EVENTS);
}

bool record_line(const DailyStatisticsRecord& r, std::size_t index, char* out, std::size_t size)
{
    int written = -1;
    if (index == 0) written = std::snprintf(out, size, "DAY:%lu", static_cast<unsigned long>(r.yyyymmdd));
    else if (index == 1) written = std::snprintf(out, size, "HYD:ML=%lu,GOAL=%lu,ACK=%lu,MISS=%lu",
        static_cast<unsigned long>(r.hydration_ml), static_cast<unsigned long>(r.hydration_goal_ml),
        static_cast<unsigned long>(r.hydration.acknowledged), static_cast<unsigned long>(r.hydration.missed));
    else if (index == 2) written = std::snprintf(out, size, "STR:ACK=%lu,MISS=%lu", static_cast<unsigned long>(r.stretch.acknowledged), static_cast<unsigned long>(r.stretch.missed));
    else if (index == 3) written = std::snprintf(out, size, "EYE:ACK=%lu,MISS=%lu", static_cast<unsigned long>(r.eye.acknowledged), static_cast<unsigned long>(r.eye.missed));
    else if (index == 4) written = std::snprintf(out, size, "WALK:ACK=%lu,MISS=%lu", static_cast<unsigned long>(r.walk.acknowledged), static_cast<unsigned long>(r.walk.missed));
    else if (index == 5) written = std::snprintf(out, size, "MEDIT:ACK=%lu,MISS=%lu", static_cast<unsigned long>(r.meditation.acknowledged), static_cast<unsigned long>(r.meditation.missed));
    else
    {
        const std::size_t med_count = used_count(r.medicines, MAX_MEDICINES);
        const std::size_t custom_count = used_count(r.custom_events, MAX_CUSTOM_EVENTS);
        if (index < 6 + med_count)
        {
            const auto* t = nth_used(r.medicines, MAX_MEDICINES, index - 6);
            if (t) written = std::snprintf(out, size, "MED:%s,A=%lu,M=%lu,S=%lu,LA=%lld,LM=%lld", t->token_id,
                static_cast<unsigned long>(t->acknowledged), static_cast<unsigned long>(t->missed), static_cast<unsigned long>(t->snoozed),
                static_cast<long long>(t->last_ack_timestamp), static_cast<long long>(t->last_miss_timestamp));
        }
        else if (index < 6 + med_count + custom_count)
        {
            const auto* t = nth_used(r.custom_events, MAX_CUSTOM_EVENTS, index - 6 - med_count);
            if (t) written = std::snprintf(out, size, "CUSTOM:%s,A=%lu,M=%lu,LA=%lld,LM=%lld", t->token_id,
                static_cast<unsigned long>(t->acknowledged), static_cast<unsigned long>(t->missed),
                static_cast<long long>(t->last_ack_timestamp), static_cast<long long>(t->last_miss_timestamp));
        }
        else if (index == 6 + med_count + custom_count)
        {
            written = std::snprintf(out, size, "DAY_END");
        }
    }
    return written >= 0 && static_cast<std::size_t>(written) < size;
}
}

esp_err_t statistics_history_init()
{
    if (initialized) return ESP_OK;
    esp_vfs_spiffs_conf_t config = {};
    config.base_path = BASE_PATH;
    config.partition_label = "spiffs";
    config.max_files = 4;
    config.format_if_mount_failed = true;

    const esp_err_t error = esp_vfs_spiffs_register(&config);
    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(error));
        return error;
    }

    if (!load_file())
    {
        reset_memory();
        if (!save_file()) return ESP_FAIL;
    }

    initialized = true;
    ESP_LOGI(TAG, "History ready: %lu/%u days", static_cast<unsigned long>(history.count), static_cast<unsigned>(STATISTICS_HISTORY_DAYS));
    return ESP_OK;
}

bool statistics_history_is_initialized() { return initialized; }

bool statistics_history_append(const DailyStatisticsRecord& input)
{
    if (!initialized || input.yyyymmdd == 0) return false;
    DailyStatisticsRecord record = input;
    record.version = 1;
    record.checksum = calculate_checksum(record);

    for (std::size_t i = 0; i < history.count; ++i)
    {
        const int p = newest_physical_index(i);
        if (p >= 0 && history.records[p].yyyymmdd == record.yyyymmdd)
        {
            history.records[p] = record;
            return save_file();
        }
    }

    history.records[history.write_index] = record;
    history.write_index = (history.write_index + 1) % STATISTICS_HISTORY_DAYS;
    if (history.count < STATISTICS_HISTORY_DAYS) ++history.count;
    selected_physical_index = -1;
    return save_file();
}

bool statistics_history_clear()
{
    if (!initialized) return false;
    reset_memory();
    return save_file();
}

std::size_t statistics_history_record_count() { return initialized ? history.count : 0; }

std::size_t statistics_history_line_count()
{
    if (!initialized) return 0;
    std::size_t total = 2; // begin + end
    for (std::size_t i = 0; i < history.count; ++i)
    {
        const int p = newest_physical_index(i);
        if (p >= 0 && valid_record(history.records[p])) total += record_line_count(history.records[p]);
    }
    return total;
}

bool statistics_history_get_line(std::size_t index, char* out, std::size_t size)
{
    if (!initialized || !out || size == 0) return false;
    if (index == 0) return std::snprintf(out, size, "HISTORY_BEGIN:v1,COUNT=%lu", static_cast<unsigned long>(history.count)) < static_cast<int>(size);
    std::size_t cursor = 1;
    for (std::size_t i = 0; i < history.count; ++i)
    {
        const int p = newest_physical_index(i);
        if (p < 0 || !valid_record(history.records[p])) continue;
        const std::size_t lines = record_line_count(history.records[p]);
        if (index < cursor + lines) return record_line(history.records[p], index - cursor, out, size);
        cursor += lines;
    }
    if (index == cursor) return std::snprintf(out, size, "HISTORY_END") < static_cast<int>(size);
    return false;
}

bool statistics_history_select_day(uint32_t day)
{
    selected_physical_index = -1;
    if (!initialized) return false;
    for (std::size_t i = 0; i < history.count; ++i)
    {
        const int p = newest_physical_index(i);
        if (p >= 0 && valid_record(history.records[p]) && history.records[p].yyyymmdd == day)
        {
            selected_physical_index = p;
            return true;
        }
    }
    return false;
}

std::size_t statistics_history_selected_day_line_count()
{
    return selected_physical_index >= 0 ? record_line_count(history.records[selected_physical_index]) + 2 : 0;
}

bool statistics_history_get_selected_day_line(std::size_t index, char* out, std::size_t size)
{
    if (selected_physical_index < 0 || !out || size == 0) return false;
    const auto& r = history.records[selected_physical_index];
    if (index == 0) return std::snprintf(out, size, "HISTORY_DAY_BEGIN:v1") < static_cast<int>(size);
    const std::size_t lines = record_line_count(r);
    if (index <= lines) return record_line(r, index - 1, out, size);
    if (index == lines + 1) return std::snprintf(out, size, "HISTORY_DAY_END") < static_cast<int>(size);
    return false;
}