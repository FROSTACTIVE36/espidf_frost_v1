#pragma once

#include <cstddef>
#include "esp_err.h"

#define FROST_FIRMWARE_VERSION "1.1.4"
#define FROST_VERSION_JSON_URL "https://github.com/Jeevuppendra/frost-ota/raw/refs/heads/main/version.json"
#define FROST_FIRMWARE_BIN_URL "https://github.com/Jeevuppendra/frost-ota/raw/refs/heads/main/firmware.bin"

esp_err_t ota_manager_init();
bool ota_manager_request_check();
bool ota_manager_request_start();
bool ota_manager_request_cancel();
bool ota_manager_is_busy();
void ota_manager_get_status(char* output, std::size_t output_size);