#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

enum class BottleCalibrationState : uint8_t
{
    IDLE,
    WAIT_REMOVE_FOR_TARE,
    TARING,
    WAIT_EMPTY_BOTTLE,
    MEASURE_EMPTY,
    WAIT_REMOVE_TO_FILL,
    WAIT_FULL_BOTTLE,
    MEASURE_FULL,
    COMPLETE,
    CANCELLED,
    ERROR
};

esp_err_t bottle_calibration_init();

bool bottle_calibration_start();
void bottle_calibration_cancel();

void bottle_calibration_set_docked(bool docked);
void bottle_calibration_update();

bool bottle_calibration_is_active();
BottleCalibrationState bottle_calibration_get_state();

void bottle_calibration_get_status(
    char* destination,
    std::size_t destination_size
);