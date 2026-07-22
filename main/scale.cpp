#include "scale.hpp"

#include "hx711.hpp"
#include "esp_log.h"

namespace
{
const char* TAG = "SCALE";
ScaleCalibration calibration = {};
bool initialized = false;
}

esp_err_t scale_init()
{
    Hx711Config config;
    config.dout_gpio = GPIO_NUM_5;
    config.sck_gpio = GPIO_NUM_6;
    config.ready_timeout_us = 3000;

    const esp_err_t result = hx711_init(config);

    if (result != ESP_OK)
    {
        return result;
    }

    initialized = true;

    ESP_LOGI(
        TAG,
        "Scale initialized, default grams/raw-unit=%.6f",
        DEFAULT_SCALE_CALIB_G_PER_UNIT
    );

    return ESP_OK;
}

bool scale_is_initialized()
{
    return initialized;
}

void scale_set_calibration(const ScaleCalibration& value)
{
    calibration = value;

    if (calibration.grams_per_raw_unit == 0.0f)
    {
        calibration.grams_per_raw_unit =
            DEFAULT_SCALE_CALIB_G_PER_UNIT;
    }
}

const ScaleCalibration& scale_get_calibration()
{
    return calibration;
}

bool scale_read_raw(int32_t& raw_value)
{
    return initialized && hx711_read_raw(raw_value);
}

float scale_raw_to_grams(int32_t raw_value)
{
    return
        static_cast<float>(raw_value - calibration.offset_raw) *
        calibration.grams_per_raw_unit;
}

bool scale_read_weight_g(float& weight_g)
{
    int32_t raw = 0;

    if (!scale_read_raw(raw))
    {
        return false;
    }

    weight_g = scale_raw_to_grams(raw);
    return true;
}