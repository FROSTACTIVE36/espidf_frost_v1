#pragma once

#include <cstddef>

#include "esp_err.h"

/*
 * Saves the complete user configuration JSON in NVS.
 */
esp_err_t configuration_storage_save(
    const char* json_text,
    std::size_t json_length
);

/*
 * Loads the saved configuration JSON.
 *
 * The returned buffer is allocated using malloc().
 * The caller must release it using free().
 */
esp_err_t configuration_storage_load(
    char** json_text,
    std::size_t* json_length
);

/*
 * Deletes the saved user configuration.
 * The device will use the default JSON on the next boot.
 */
esp_err_t configuration_storage_clear();