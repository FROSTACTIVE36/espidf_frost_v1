#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * Initialize the I2C bus and DS3231.
 *
 * SDA: GPIO 8
 * SCL: GPIO 9
 */
esp_err_t rtc_ds3231_init(void);

/**
 * Read the DS3231 and set the ESP32 system clock.
 */
esp_err_t rtc_ds3231_sync_system_time(void);

/**
 * Parse and apply:
 *
 * SET 2026-07-20 22:30:00
 *
 * This writes the time to the DS3231 and then synchronizes
 * the ESP32 internal system time.
 */
bool rtc_ds3231_process_set_command(const char *command);

/**
 * Return current DS3231 time as:
 *
 * 2026-07-20 22:30:00
 */
bool rtc_ds3231_get_time_string(
    char *output,
    size_t output_size
);