#include "wifi_manager.hpp"

#include <cstring>
#include <cstdio>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

namespace
{
constexpr char TAG[] = "WIFI_MANAGER";
constexpr char NVS_NAMESPACE[] = "frost_wifi";
constexpr char NVS_SSID_KEY[] = "ssid";
constexpr char NVS_PASSWORD_KEY[] = "password";
constexpr EventBits_t CONNECTED_BIT = BIT0;
constexpr EventBits_t FAILED_BIT = BIT1;

EventGroupHandle_t event_group = nullptr;
bool initialized = false;
bool credentials_available = false;
char ssid[33] = {};
char password[65] = {};

void load_credentials()
{
    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return;
    }

    size_t ssid_length = sizeof(ssid);
    size_t password_length = sizeof(password);
    const esp_err_t ssid_result = nvs_get_str(handle, NVS_SSID_KEY, ssid, &ssid_length);
    const esp_err_t password_result = nvs_get_str(handle, NVS_PASSWORD_KEY, password, &password_length);
    nvs_close(handle);

    credentials_available =
        ssid_result == ESP_OK &&
        password_result == ESP_OK &&
        ssid[0] != '\0';
}

void event_handler(void*, esp_event_base_t base, int32_t id, void*)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        xEventGroupClearBits(event_group, CONNECTED_BIT);
        xEventGroupSetBits(event_group, FAILED_BIT);
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupClearBits(event_group, FAILED_BIT);
        xEventGroupSetBits(event_group, CONNECTED_BIT);
    }
}

bool apply_station_config()
{
    if (!credentials_available)
    {
        return false;
    }

    wifi_config_t config = {};
    std::strncpy(reinterpret_cast<char*>(config.sta.ssid), ssid, sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(config.sta.password), password, sizeof(config.sta.password) - 1);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    return esp_wifi_set_config(WIFI_IF_STA, &config) == ESP_OK;
}
}

esp_err_t wifi_manager_init()
{
    if (initialized)
    {
        return ESP_OK;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_netif_init());

    esp_err_t loop_result = esp_event_loop_create_default();
    if (loop_result != ESP_OK && loop_result != ESP_ERR_INVALID_STATE)
    {
        return loop_result;
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result = esp_wifi_init(&wifi_init);
    if (result != ESP_OK)
    {
        return result;
    }

    event_group = xEventGroupCreate();
    if (event_group == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, nullptr)
    );
    ESP_ERROR_CHECK_WITHOUT_ABORT(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, nullptr)
    );

    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_mode(WIFI_MODE_STA));

    result = esp_wifi_start();
    if (result != ESP_OK)
    {
        return result;
    }

    load_credentials();
    if (credentials_available)
    {
        apply_station_config();
    }

    initialized = true;
    ESP_LOGI(TAG, "Wi-Fi manager ready; credentials=%s", credentials_available ? "yes" : "no");
    return ESP_OK;
}

bool wifi_manager_set_credentials(const char* new_ssid, const char* new_password)
{
    if (new_ssid == nullptr || new_password == nullptr)
    {
        return false;
    }

    const size_t ssid_length = std::strlen(new_ssid);
    const size_t password_length = std::strlen(new_password);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 64)
    {
        return false;
    }

    nvs_handle_t handle = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK)
    {
        return false;
    }

    esp_err_t result = nvs_set_str(handle, NVS_SSID_KEY, new_ssid);
    if (result == ESP_OK)
    {
        result = nvs_set_str(handle, NVS_PASSWORD_KEY, new_password);
    }
    if (result == ESP_OK)
    {
        result = nvs_commit(handle);
    }
    nvs_close(handle);

    if (result != ESP_OK)
    {
        return false;
    }

    std::strncpy(ssid, new_ssid, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';
    std::strncpy(password, new_password, sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    credentials_available = true;

    if (initialized)
    {
        esp_wifi_disconnect();
        apply_station_config();
    }

    return true;
}

bool wifi_manager_has_credentials()
{
    return credentials_available;
}

bool wifi_manager_connect(unsigned timeout_ms)
{
    if (!initialized || !credentials_available || event_group == nullptr)
    {
        return false;
    }

    if (wifi_manager_is_connected())
    {
        return true;
    }

    xEventGroupClearBits(event_group, CONNECTED_BIT | FAILED_BIT);
    if (!apply_station_config())
    {
        return false;
    }

    esp_wifi_connect();

    const EventBits_t bits = xEventGroupWaitBits(
        event_group,
        CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(timeout_ms)
    );

    return (bits & CONNECTED_BIT) != 0;
}

void wifi_manager_disconnect()
{
    if (initialized)
    {
        esp_wifi_disconnect();
    }
}

bool wifi_manager_is_connected()
{
    if (!initialized || event_group == nullptr)
    {
        return false;
    }

    return (xEventGroupGetBits(event_group) & CONNECTED_BIT) != 0;
}

void wifi_manager_get_status(char* output, std::size_t output_size)
{
    if (output == nullptr || output_size == 0)
    {
        return;
    }

    std::snprintf(
        output,
        output_size,
        "WIFI:credentials=%s,connected=%s,ssid=%s",
        credentials_available ? "yes" : "no",
        wifi_manager_is_connected() ? "yes" : "no",
        credentials_available ? ssid : ""
    );
}