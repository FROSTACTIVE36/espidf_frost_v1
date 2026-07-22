#pragma once

#include <cstdint>

#include "driver/gpio.h"
#include "esp_err.h"

struct Hx711Config
{
    gpio_num_t dout_gpio = GPIO_NUM_5;
    gpio_num_t sck_gpio = GPIO_NUM_6;
    uint32_t ready_timeout_us = 3000;
};

esp_err_t hx711_init(const Hx711Config& config);
bool hx711_is_initialized();
bool hx711_is_ready();

/*
 * Reads one signed 24-bit conversion.
 * Returns false if DOUT did not become ready before timeout.
 */
bool hx711_read_raw(int32_t& raw_value);

/*
 * Sends 25 clock pulses total, selecting channel A gain 128.
 */
void hx711_power_down();
void hx711_power_up();