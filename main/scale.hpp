#pragma once

#include <cstdint>

#include "esp_err.h"

static constexpr float DEFAULT_SCALE_CALIB_G_PER_UNIT = 0.023125f;

struct ScaleCalibration
{
    bool valid = false;
    int32_t offset_raw = 0;
    float grams_per_raw_unit = DEFAULT_SCALE_CALIB_G_PER_UNIT;
    float empty_bottle_g = 0.0f;
    float full_bottle_g = 0.0f;
    float capacity_ml = 0.0f;
};

esp_err_t scale_init();
bool scale_is_initialized();

void scale_set_calibration(const ScaleCalibration& calibration);
const ScaleCalibration& scale_get_calibration();

bool scale_read_raw(int32_t& raw_value);
bool scale_read_weight_g(float& weight_g);

float scale_raw_to_grams(int32_t raw_value);