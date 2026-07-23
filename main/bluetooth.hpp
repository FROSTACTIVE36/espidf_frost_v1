#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/**
 * Function called when a complete JSON configuration
 * has been received.
 *
 * Return true when the JSON was parsed and applied.
 */
using bluetooth_json_handler_t =
    bool (*)(
        const char *json_text,
        size_t json_length
    );

/**
 * Initialize NimBLE and start advertising.
 *
 * json_handler will be called after:
 *
 * JSON_BEGIN
 * JSON_CHUNK:...
 * JSON_END
 */
esp_err_t bluetooth_init(
    bluetooth_json_handler_t json_handler
);

/**
 * Returns true after BLE initialization succeeds.
 */
bool bluetooth_is_initialized(void);

/**
 * Returns true once when BLE has queued a bottle-calibration start request.
 * Call from the main application task; do not draw from the NimBLE host task.
 */
bool bluetooth_take_bottle_calibration_start_request(void);

/**
 * Returns true once when BLE has queued a bottle-calibration cancel request.
 * Call from the main application task.
 */
bool bluetooth_take_bottle_calibration_cancel_request(void);