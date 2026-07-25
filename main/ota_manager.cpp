#include "ota_manager.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "action_log.hpp"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "wifi_manager.hpp"

namespace
{
constexpr char TAG[] = "OTA_MANAGER";
constexpr size_t VERSION_RESPONSE_MAX = 2048;

enum class Request : uint8_t { CHECK, START, CANCEL };
QueueHandle_t request_queue = nullptr;
TaskHandle_t task_handle = nullptr;
std::atomic_bool busy{false};
std::atomic_bool cancel_requested{false};
char status_text[128] = "OTA:READY";
portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;

void set_status(const char* text)
{
    taskENTER_CRITICAL(&status_lock);
    std::strncpy(status_text, text != nullptr ? text : "", sizeof(status_text) - 1);
    status_text[sizeof(status_text) - 1] = '\0';
    taskEXIT_CRITICAL(&status_lock);
    ESP_LOGI(TAG, "%s", status_text);
}

struct HttpBuffer
{
    char data[VERSION_RESPONSE_MAX] = {};
    size_t length = 0;
};

esp_err_t http_event_handler(esp_http_client_event_t* event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->user_data == nullptr || event->data == nullptr)
    {
        return ESP_OK;
    }

    auto* buffer = static_cast<HttpBuffer*>(event->user_data);
    const size_t available = sizeof(buffer->data) - 1 - buffer->length;
    const size_t copy_length = static_cast<size_t>(event->data_len) < available
        ? static_cast<size_t>(event->data_len)
        : available;

    std::memcpy(buffer->data + buffer->length, event->data, copy_length);
    buffer->length += copy_length;
    buffer->data[buffer->length] = '\0';
    return ESP_OK;
}

bool fetch_version_json(char* version, size_t version_size, char* firmware_url, size_t url_size)
{
    HttpBuffer response;
    esp_http_client_config_t config = {};
    config.url = FROST_VERSION_JSON_URL;
    config.event_handler = http_event_handler;
    config.user_data = &response;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 20000;
    config.disable_auto_redirect = false;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr)
    {
        return false;
    }

    const esp_err_t result = esp_http_client_perform(client);
    const int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (result != ESP_OK || status_code < 200 || status_code >= 300)
    {
        return false;
    }

    cJSON* root = cJSON_Parse(response.data);
    if (root == nullptr)
    {
        return false;
    }

    const cJSON* version_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON* url_item = cJSON_GetObjectItemCaseSensitive(root, "firmware_url");

    bool success = cJSON_IsString(version_item) && version_item->valuestring != nullptr;
    if (success)
    {
        std::strncpy(version, version_item->valuestring, version_size - 1);
        version[version_size - 1] = '\0';

        const char* selected_url =
            cJSON_IsString(url_item) && url_item->valuestring != nullptr
                ? url_item->valuestring
                : FROST_FIRMWARE_BIN_URL;
        std::strncpy(firmware_url, selected_url, url_size - 1);
        firmware_url[url_size - 1] = '\0';
    }

    cJSON_Delete(root);
    return success;
}

int compare_versions(const char* left, const char* right)
{
    int l[3] = {};
    int r[3] = {};
    std::sscanf(left != nullptr ? left : "0", "%d.%d.%d", &l[0], &l[1], &l[2]);
    std::sscanf(right != nullptr ? right : "0", "%d.%d.%d", &r[0], &r[1], &r[2]);

    for (int index = 0; index < 3; ++index)
    {
        if (l[index] != r[index])
        {
            return l[index] < r[index] ? -1 : 1;
        }
    }
    return 0;
}

bool prepare_update(char* firmware_url, size_t firmware_url_size, bool& update_available)
{
    action_log_show_ota("WiFi connecting...", true);
    set_status("OTA:WIFI_CONNECTING");

    if (!wifi_manager_connect(45000))
    {
        action_log_show_ota_result("WiFi connection failed", 3500);
        set_status("OTA:ERROR:WIFI");
        return false;
    }

    action_log_show_ota("Checking update...", true);
    set_status("OTA:CHECKING");

    char remote_version[32] = {};
    if (!fetch_version_json(remote_version, sizeof(remote_version), firmware_url, firmware_url_size))
    {
        action_log_show_ota_result("Update check failed", 3500);
        set_status("OTA:ERROR:VERSION_JSON");
        return false;
    }

    update_available = compare_versions(FROST_FIRMWARE_VERSION, remote_version) < 0;

    char response[128] = {};
    std::snprintf(
        response,
        sizeof(response),
        "OTA:current=%s,remote=%s,available=%s",
        FROST_FIRMWARE_VERSION,
        remote_version,
        update_available ? "yes" : "no"
    );
    set_status(response);
    return true;
}

bool perform_update(const char* firmware_url)
{
    action_log_show_ota_progress(0);
    set_status("OTA:DOWNLOADING:0");

    /*
     * Keep the HTTP buffers small because Wi-Fi, BLE, TLS and the display
     * are active at the same time. The OTA receive buffer itself is placed
     * in PSRAM below.
     */
    esp_http_client_config_t http_config = {};
    http_config.url = firmware_url;
    http_config.crt_bundle_attach = esp_crt_bundle_attach;
    http_config.timeout_ms = 30000;
    http_config.keep_alive_enable = false;
    http_config.buffer_size = 2048;
    http_config.buffer_size_tx = 1024;
    http_config.disable_auto_redirect = false;
    http_config.max_redirection_count = 5;

    esp_https_ota_config_t ota_config = {};
    ota_config.http_config = &http_config;

    /*
     * ESP-IDF 6.x supports selecting the allocation capabilities used by
     * the OTA buffer. Use the already initialized external PSRAM.
     */
    ota_config.buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

#if CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD
    /*
     * Partial download reduces TLS receive-buffer pressure by downloading
     * the image through smaller HTTP range requests.
     */
    ota_config.partial_http_download = true;
    ota_config.max_http_request_size = 4096;
#endif

    const size_t internal_free =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t internal_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t psram_free =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t psram_largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    ESP_LOGI(
        TAG,
        "OTA heap before begin: internal=%u largest_internal=%u "
        "psram=%u largest_psram=%u",
        static_cast<unsigned>(internal_free),
        static_cast<unsigned>(internal_largest),
        static_cast<unsigned>(psram_free),
        static_cast<unsigned>(psram_largest)
    );

#if CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD
    ESP_LOGI(TAG, "Partial HTTP OTA enabled; request size=4096");
#else
    ESP_LOGW(
        TAG,
        "Partial HTTP OTA is disabled. Enable "
        "CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD to reduce TLS memory use."
    );
#endif

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t result = esp_https_ota_begin(&ota_config, &handle);
    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_https_ota_begin failed: %s (0x%x)",
            esp_err_to_name(result),
            static_cast<unsigned>(result)
        );
        set_status("OTA:ERROR:BEGIN");
        return false;
    }

    const int image_size = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Remote OTA image size: %d bytes", image_size);

    int last_percentage = -1;

    while (true)
    {
        if (cancel_requested.load())
        {
            esp_https_ota_abort(handle);
            set_status("OTA:CANCELLED");
            return false;
        }

        result = esp_https_ota_perform(handle);
        const int read_size = esp_https_ota_get_image_len_read(handle);

        if (image_size > 0 && read_size >= 0)
        {
            const int percentage = (read_size * 100) / image_size;
            if (percentage != last_percentage)
            {
                last_percentage = percentage;
                action_log_show_ota_progress(percentage);

                char progress_status[48] = {};
                std::snprintf(
                    progress_status,
                    sizeof(progress_status),
                    "OTA:DOWNLOADING:%d",
                    percentage
                );
                set_status(progress_status);
            }
        }

        if (result != ESP_ERR_HTTPS_OTA_IN_PROGRESS)
        {
            break;
        }
    }

    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_https_ota_perform failed: %s (0x%x), bytes_read=%d",
            esp_err_to_name(result),
            static_cast<unsigned>(result),
            esp_https_ota_get_image_len_read(handle)
        );
        esp_https_ota_abort(handle);
        set_status("OTA:ERROR:DOWNLOAD");
        return false;
    }

    action_log_show_ota("Verifying firmware...", true);
    set_status("OTA:VERIFYING");

    result = esp_https_ota_finish(handle);
    if (result != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "esp_https_ota_finish failed: %s (0x%x)",
            esp_err_to_name(result),
            static_cast<unsigned>(result)
        );
        set_status("OTA:ERROR:FINISH");
        return false;
    }

    return true;
}

void ota_task(void*)
{
    for (;;)
    {
        Request request;
        if (xQueueReceive(request_queue, &request, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (request == Request::CANCEL)
        {
            cancel_requested.store(true);
            continue;
        }

        if (busy.exchange(true))
        {
            continue;
        }

        cancel_requested.store(false);

        char firmware_url[320] = {};
        bool update_available = false;
        const bool prepared = prepare_update(firmware_url, sizeof(firmware_url), update_available);

        if (!prepared)
        {
            busy.store(false);
            continue;
        }

        if (!update_available)
        {
            action_log_show_ota_result("No update available", 3000);
            busy.store(false);
            continue;
        }

        if (request == Request::CHECK)
        {
            action_log_show_ota_result("Update available", 3000);
            busy.store(false);
            continue;
        }

        /*
         * For a START request, transition directly from "Checking update..."
         * to "Downloading 0%" inside perform_update(). This keeps the
         * Action Log continuous and avoids an unnecessary intermediate state.
         */
        if (!perform_update(firmware_url))
        {
            if (cancel_requested.load())
            {
                action_log_show_ota_result("Update cancelled", 3000);
            }
            else
            {
                action_log_show_ota_result("OTA failed", 3500);
            }
            busy.store(false);
            continue;
        }

        set_status("OTA:COMPLETE");
        action_log_show_ota_result("Update complete", 1200);
        vTaskDelay(pdMS_TO_TICKS(1200));
        action_log_show_ota_result("Restarting...", 800);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
}
}

esp_err_t ota_manager_init()
{
    if (request_queue != nullptr)
    {
        return ESP_OK;
    }

    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY)
    {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_ota_mark_app_valid_cancel_rollback());
    }

    request_queue = xQueueCreate(4, sizeof(Request));
    if (request_queue == nullptr)
    {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Keep OTA work away from the main UI task.
     *
     * app_main and all LovyanGFX rendering run on Core 0. During version
     * checking and esp_https_ota_begin(), TLS/HTTP calls may remain runnable
     * for long periods. A higher-priority OTA task on the same core can then
     * starve the display task and make the Action Log appear frozen.
     *
     * Run OTA on Core 1 at a moderate priority so the display continues
     * refreshing smoothly during:
     *
     * WiFi connecting -> Checking update -> Downloading.
     */
    if (
        xTaskCreatePinnedToCore(
            ota_task,
            "ota_task",
            12288,
            nullptr,
            3,
            &task_handle,
            1
        ) != pdPASS
    )
    {
        vQueueDelete(request_queue);
        request_queue = nullptr;
        return ESP_ERR_NO_MEM;
    }

    set_status("OTA:READY");
    return ESP_OK;
}

bool ota_manager_request_check()
{
    if (request_queue == nullptr || busy.load()) return false;
    const Request request = Request::CHECK;
    return xQueueSend(request_queue, &request, 0) == pdTRUE;
}

bool ota_manager_request_start()
{
    if (request_queue == nullptr || busy.load()) return false;
    const Request request = Request::START;
    return xQueueSend(request_queue, &request, 0) == pdTRUE;
}

bool ota_manager_request_cancel()
{
    if (!busy.load()) return false;
    cancel_requested.store(true);
    return true;
}

bool ota_manager_is_busy()
{
    return busy.load();
}

void ota_manager_get_status(char* output, std::size_t output_size)
{
    if (output == nullptr || output_size == 0) return;
    taskENTER_CRITICAL(&status_lock);
    std::strncpy(output, status_text, output_size - 1);
    output[output_size - 1] = '\0';
    taskEXIT_CRITICAL(&status_lock);
}