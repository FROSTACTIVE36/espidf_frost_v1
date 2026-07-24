#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "reminder_types.hpp"

static constexpr std::size_t STATISTICS_HISTORY_DAYS = 30;

struct HistoryBasicCounter
{
    uint32_t acknowledged = 0;
    uint32_t missed = 0;
};

struct HistoryTokenCounter
{
    bool used = false;
    char token_id[REMINDER_ID_LENGTH] = {};
    uint32_t acknowledged = 0;
    uint32_t missed = 0;
    uint32_t snoozed = 0;
    int64_t last_ack_timestamp = 0;
    int64_t last_miss_timestamp = 0;
};

struct DailyStatisticsRecord
{
    uint32_t version = 1;
    uint32_t yyyymmdd = 0;
    uint32_t hydration_goal_ml = 0;
    uint32_t hydration_ml = 0;

    HistoryBasicCounter hydration = {};
    HistoryBasicCounter stretch = {};
    HistoryBasicCounter eye = {};
    HistoryBasicCounter walk = {};
    HistoryBasicCounter meditation = {};

    HistoryTokenCounter medicines[MAX_MEDICINES] = {};
    HistoryTokenCounter custom_events[MAX_CUSTOM_EVENTS] = {};

    uint32_t checksum = 0;
};

esp_err_t statistics_history_init();
bool statistics_history_is_initialized();
bool statistics_history_append(const DailyStatisticsRecord& record);
bool statistics_history_clear();
std::size_t statistics_history_record_count();

/* BLE flattened exports. newest_first=true is used for the complete history. */
std::size_t statistics_history_line_count();
bool statistics_history_get_line(std::size_t index, char* output, std::size_t output_size);

bool statistics_history_select_day(uint32_t yyyymmdd);
std::size_t statistics_history_selected_day_line_count();
bool statistics_history_get_selected_day_line(std::size_t index, char* output, std::size_t output_size);