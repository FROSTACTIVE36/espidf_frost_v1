#pragma once

#include <cstddef>
#include "esp_err.h"

esp_err_t wifi_manager_init();

bool wifi_manager_set_credentials(const char* ssid, const char* password);
bool wifi_manager_has_credentials();
bool wifi_manager_connect(unsigned timeout_ms = 45000);
void wifi_manager_disconnect();

bool wifi_manager_is_connected();
void wifi_manager_get_status(char* output, std::size_t output_size);