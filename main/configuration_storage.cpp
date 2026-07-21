#include "configuration_storage.hpp"

#include <cstdlib>
#include <cstring>

#include "esp_log.h"
#include "nvs.h"

static const char* TAG = "CONFIG_STORAGE";

static constexpr const char* NVS_NAMESPACE =
    "frost_config";

static constexpr const char* NVS_JSON_KEY =
    "user_json";

/*
 * Maximum accepted JSON size.
 *
 * Increase this if your Bluetooth configuration becomes larger.
 */
static constexpr std::size_t MAX_CONFIG_JSON_SIZE =
    32 * 1024;

esp_err_t configuration_storage_save(
    const char* json_text,
    std::size_t json_length
)
{
    if (
        json_text == nullptr ||
        json_length == 0
    )
    {
        ESP_LOGE(
            TAG,
            "Cannot save empty configuration"
        );

        return ESP_ERR_INVALID_ARG;
    }

    if (json_length > MAX_CONFIG_JSON_SIZE)
    {
        ESP_LOGE(
            TAG,
            "Configuration too large: %u bytes",
            static_cast<unsigned int>(
                json_length
            )
        );

        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle = 0;

    esp_err_t error =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to open NVS: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    error =
        nvs_set_blob(
            handle,
            NVS_JSON_KEY,
            json_text,
            json_length
        );

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to write JSON: %s",
            esp_err_to_name(error)
        );

        nvs_close(handle);
        return error;
    }

    /*
     * nvs_commit() is required to make the write persistent.
     */
    error =
        nvs_commit(handle);

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Failed to commit JSON: %s",
            esp_err_to_name(error)
        );

        nvs_close(handle);
        return error;
    }

    nvs_close(handle);

    ESP_LOGI(
        TAG,
        "Saved user configuration: %u bytes",
        static_cast<unsigned int>(
            json_length
        )
    );

    return ESP_OK;
}

esp_err_t configuration_storage_load(
    char** json_text,
    std::size_t* json_length
)
{
    if (
        json_text == nullptr ||
        json_length == nullptr
    )
    {
        return ESP_ERR_INVALID_ARG;
    }

    *json_text = nullptr;
    *json_length = 0;

    nvs_handle_t handle = 0;

    esp_err_t error =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READONLY,
            &handle
        );

    if (error != ESP_OK)
    {
        /*
         * ESP_ERR_NVS_NOT_FOUND means no saved configuration exists yet.
         */
        return error;
    }

    std::size_t stored_size = 0;

    /*
     * First query the required blob size.
     */
    error =
        nvs_get_blob(
            handle,
            NVS_JSON_KEY,
            nullptr,
            &stored_size
        );

    if (error != ESP_OK)
    {
        nvs_close(handle);
        return error;
    }

    if (
        stored_size == 0 ||
        stored_size > MAX_CONFIG_JSON_SIZE
    )
    {
        ESP_LOGE(
            TAG,
            "Invalid saved JSON size: %u",
            static_cast<unsigned int>(
                stored_size
            )
        );

        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * Add one extra byte for the C-string terminator.
     */
    char* buffer =
        static_cast<char*>(
            std::malloc(stored_size + 1)
        );

    if (buffer == nullptr)
    {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    std::size_t read_size =
        stored_size;

    error =
        nvs_get_blob(
            handle,
            NVS_JSON_KEY,
            buffer,
            &read_size
        );

    nvs_close(handle);

    if (error != ESP_OK)
    {
        std::free(buffer);
        return error;
    }

    buffer[read_size] = '\0';

    *json_text = buffer;
    *json_length = read_size;

    ESP_LOGI(
        TAG,
        "Loaded saved configuration: %u bytes",
        static_cast<unsigned int>(
            read_size
        )
    );

    return ESP_OK;
}

esp_err_t configuration_storage_clear()
{
    nvs_handle_t handle = 0;

    esp_err_t error =
        nvs_open(
            NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (error != ESP_OK)
    {
        return error;
    }

    error =
        nvs_erase_key(
            handle,
            NVS_JSON_KEY
        );

    if (
        error != ESP_OK &&
        error != ESP_ERR_NVS_NOT_FOUND
    )
    {
        nvs_close(handle);
        return error;
    }

    error =
        nvs_commit(handle);

    nvs_close(handle);

    if (error == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Saved configuration cleared"
        );
    }

    return error;
}