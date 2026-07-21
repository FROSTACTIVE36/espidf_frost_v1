#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

/*
 * Active-low IR acknowledgement input.
 *
 * Acknowledgement is generated only on a new HIGH -> LOW transition.
 * Keeping a hand/object in front of the sensor does not generate repeated ACKs.
 */
using acknowledgement_callback_t = void (*)();

struct AcknowledgementInputConfig
{
    gpio_num_t gpio = GPIO_NUM_7;
    bool active_low = true;
    unsigned debounce_ms = 50;
};

esp_err_t acknowledgement_input_init(
    const AcknowledgementInputConfig& config,
    acknowledgement_callback_t callback
);

/*
 * Call regularly from the application loop.
 * A 10-25 ms update interval is suitable.
 */
void acknowledgement_input_update();

bool acknowledgement_input_is_initialized();