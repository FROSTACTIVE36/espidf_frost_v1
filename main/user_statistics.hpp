#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "reminder_types.hpp"

esp_err_t user_statistics_init();
void user_statistics_update_day();

void user_statistics_record_ack(
    ReminderType type,
    const char* token_id = nullptr
);

void user_statistics_record_miss(
    ReminderType type,
    const char* token_id = nullptr
);

void user_statistics_record_medication_snooze(
    const char* token_id
);

void user_statistics_add_hydration_ml(uint32_t amount_ml);

/* Clears only today's hydration millilitres; other statistics are preserved. */
void user_statistics_reset_hydration_ml();

void user_statistics_reset_today();

/*
 * BLE-friendly, line-by-line export. Lines are always shorter than 128 bytes.
 * Use index 0..user_statistics_line_count()-1.
 */
std::size_t user_statistics_line_count();
bool user_statistics_get_line(
    std::size_t index,
    char* output,
    std::size_t output_size
);