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