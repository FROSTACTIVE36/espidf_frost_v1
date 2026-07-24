#include "bluetooth.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <cstddef>
#include <cstdlib>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <cstdio>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "rtc_ds3231.hpp"
#include "pomodoro.hpp"
#include "bottle_calibration.hpp"
#include "user_statistics.hpp"
#include "statistics_history.hpp"
#include "wifi_manager.hpp"
#include "ota_manager.hpp"
#include <cstring>


static const char *TAG = "FROST_BLE";


/* =========================================================
 * BLE configuration
 * ========================================================= */

#define BLE_DEFAULT_DEVICE_NAME "FROST"
#define BLE_DEVICE_NAME_MAX_LEN 24
#define BLE_DEVICE_NVS_NAMESPACE "frost_device"
#define BLE_DEVICE_NAME_NVS_KEY "ble_name"

#define BLE_COMMAND_MAX_LEN   128
#define JSON_CONFIG_MAX_LEN   8192
#define JSON_WORKER_STACK_SIZE 8192
#define JSON_WORKER_PRIORITY   5


/*
 * Service UUID:
 *
 * DEBC9A78-5634-12EF-CDAB-89674523F14A
 */
static const ble_uuid128_t frost_service_uuid =
    BLE_UUID128_INIT(
        0x4A, 0xF1, 0x23, 0x45,
        0x67, 0x89, 0xAB, 0xCD,
        0xEF, 0x12, 0x34, 0x56,
        0x78, 0x9A, 0xBC, 0xDE
    );


/*
 * Characteristic UUID:
 *
 * 4AF12345-6789-ABCD-EF12-3456789ABCDE
 */
static const ble_uuid128_t frost_command_uuid =
    BLE_UUID128_INIT(
        0xDE, 0xBC, 0x9A, 0x78,
        0x56, 0x34, 0x12, 0xEF,
        0xCD, 0xAB, 0x89, 0x67,
        0x45, 0x23, 0xF1, 0x4A
    );


static uint8_t own_address_type = 0;
static uint16_t command_value_handle = 0;

static bool ble_initialized = false;

/*
 * The user-defined BLE name is stored separately from reminder JSON.
 * It is loaded from NVS before NimBLE advertising starts.
 */
static char current_ble_device_name[BLE_DEVICE_NAME_MAX_LEN + 1] =
    BLE_DEFAULT_DEVICE_NAME;
static std::size_t statistics_line_cursor = 0;
enum class StatisticsExportMode : uint8_t { TODAY, HISTORY, SELECTED_DAY };
static StatisticsExportMode statistics_export_mode = StatisticsExportMode::TODAY;

/*
 * BLE callbacks run in the NimBLE host task. Calibration changes the display,
 * so commands are queued here and consumed by the main application task.
 */
static std::atomic_bool bottle_calibration_start_requested{false};
static std::atomic_bool bottle_calibration_cancel_requested{false};

static bluetooth_json_handler_t
    json_configuration_handler = nullptr;


/* =========================================================
 * JSON reception state
 * ========================================================= */

static char json_configuration_buffer[
    JSON_CONFIG_MAX_LEN
];

static size_t json_configuration_length = 0;
static bool json_reception_active = false;

/*
 * The NimBLE host callback must remain lightweight. Completed JSON is copied
 * into this worker buffer and parsed by a separate FreeRTOS task.
 */
static char json_worker_buffer[JSON_CONFIG_MAX_LEN];
static size_t json_worker_length = 0;
static bool json_worker_busy = false;
static TaskHandle_t json_worker_task_handle = nullptr;
static portMUX_TYPE json_worker_lock = portMUX_INITIALIZER_UNLOCKED;


/* =========================================================
 * Status returned when characteristic is read
 * ========================================================= */

static char last_ble_status[128] =
    "FROST_BLE_READY";



/* =========================================================
 * Device identity: BLE name and Bluetooth MAC
 * ========================================================= */

static bool is_valid_device_name(
    const char *name
)
{
    if (name == nullptr)
    {
        return false;
    }

    const size_t length = strlen(name);

    if (
        length == 0 ||
        length > BLE_DEVICE_NAME_MAX_LEN
    )
    {
        return false;
    }

    for (size_t index = 0; index < length; ++index)
    {
        const unsigned char value =
            static_cast<unsigned char>(name[index]);

        /*
         * Keep the advertised name printable and single-line.
         */
        if (value < 0x20 || value > 0x7E)
        {
            return false;
        }
    }

    return true;
}


static void load_ble_device_name(void)
{
    snprintf(
        current_ble_device_name,
        sizeof(current_ble_device_name),
        "%s",
        BLE_DEFAULT_DEVICE_NAME
    );

    nvs_handle_t handle = 0;

    const esp_err_t open_error =
        nvs_open(
            BLE_DEVICE_NVS_NAMESPACE,
            NVS_READONLY,
            &handle
        );

    if (open_error == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGI(
            TAG,
            "No saved BLE device name; using default: %s",
            current_ble_device_name
        );

        return;
    }

    if (open_error != ESP_OK)
    {
        ESP_LOGW(
            TAG,
            "Could not open BLE device-name NVS: %s",
            esp_err_to_name(open_error)
        );

        return;
    }

    size_t required_length =
        sizeof(current_ble_device_name);

    const esp_err_t read_error =
        nvs_get_str(
            handle,
            BLE_DEVICE_NAME_NVS_KEY,
            current_ble_device_name,
            &required_length
        );

    nvs_close(handle);

    if (
        read_error != ESP_OK ||
        !is_valid_device_name(current_ble_device_name)
    )
    {
        snprintf(
            current_ble_device_name,
            sizeof(current_ble_device_name),
            "%s",
            BLE_DEFAULT_DEVICE_NAME
        );

        ESP_LOGW(
            TAG,
            "Saved BLE name invalid or unavailable; using default: %s",
            current_ble_device_name
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Loaded BLE device name: %s",
        current_ble_device_name
    );
}


static bool save_ble_device_name(
    const char *name
)
{
    if (!is_valid_device_name(name))
    {
        return false;
    }

    nvs_handle_t handle = 0;

    esp_err_t error =
        nvs_open(
            BLE_DEVICE_NVS_NAMESPACE,
            NVS_READWRITE,
            &handle
        );

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not open BLE device-name NVS: %s",
            esp_err_to_name(error)
        );

        return false;
    }

    error =
        nvs_set_str(
            handle,
            BLE_DEVICE_NAME_NVS_KEY,
            name
        );

    if (error == ESP_OK)
    {
        error = nvs_commit(handle);
    }

    nvs_close(handle);

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not save BLE device name: %s",
            esp_err_to_name(error)
        );

        return false;
    }

    snprintf(
        current_ble_device_name,
        sizeof(current_ble_device_name),
        "%s",
        name
    );

    /*
     * The current connection keeps working. The new name is used the
     * next time advertising starts, normally after disconnect/reconnect.
     */
    const int gap_result =
        ble_svc_gap_device_name_set(
            current_ble_device_name
        );

    if (gap_result != 0)
    {
        ESP_LOGE(
            TAG,
            "Saved name but could not update GAP name, rc=%d",
            gap_result
        );

        return false;
    }

    ESP_LOGI(
        TAG,
        "BLE device renamed to: %s",
        current_ble_device_name
    );

    return true;
}


static bool get_bluetooth_mac_string(
    char *output,
    size_t output_size
)
{
    if (
        output == nullptr ||
        output_size < 18
    )
    {
        return false;
    }

    uint8_t mac[6] = {};

    const esp_err_t error =
        esp_read_mac(
            mac,
            ESP_MAC_BT
        );

    if (error != ESP_OK)
    {
        output[0] = '\0';

        ESP_LOGE(
            TAG,
            "Could not read Bluetooth MAC: %s",
            esp_err_to_name(error)
        );

        return false;
    }

    const int written =
        snprintf(
            output,
            output_size,
            "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5]
        );

    return
        written == 17;
}


static void set_ble_status(
    const char *status
)
{
    if (status == nullptr)
    {
        return;
    }

    strncpy(
        last_ble_status,
        status,
        sizeof(last_ble_status) - 1
    );

    last_ble_status[
        sizeof(last_ble_status) - 1
    ] = '\0';

    ESP_LOGI(
        TAG,
        "Status: %s",
        last_ble_status
    );
}


/* =========================================================
 * JSON reception
 * ========================================================= */

static void reset_json_reception(void)
{
    memset(
        json_configuration_buffer,
        0,
        sizeof(json_configuration_buffer)
    );

    json_configuration_length = 0;
    json_reception_active = false;
}


static void begin_json_reception(void)
{
    reset_json_reception();

    json_reception_active = true;

    set_ble_status("OK:JSON_BEGIN");

    ESP_LOGI(
        TAG,
        "JSON reception started"
    );
}


static bool append_json_chunk(
    const char *chunk
)
{
    if (!json_reception_active)
    {
        set_ble_status(
            "ERROR:JSON_BEGIN_REQUIRED"
        );

        return false;
    }

    if (chunk == nullptr)
    {
        set_ble_status(
            "ERROR:NULL_JSON_CHUNK"
        );

        return false;
    }

    size_t chunk_length = strlen(chunk);

    if (chunk_length == 0)
    {
        return true;
    }

    /*
     * Keep one byte for '\0'.
     */
    if (
        json_configuration_length +
        chunk_length >=
        sizeof(json_configuration_buffer)
    )
    {
        ESP_LOGE(
            TAG,
            "JSON buffer overflow"
        );

        reset_json_reception();

        set_ble_status(
            "ERROR:JSON_TOO_LARGE"
        );

        return false;
    }

    memcpy(
        &json_configuration_buffer[
            json_configuration_length
        ],
        chunk,
        chunk_length
    );

    json_configuration_length +=
        chunk_length;

    json_configuration_buffer[
        json_configuration_length
    ] = '\0';

    ESP_LOGI(
        TAG,
        "JSON chunk received, total=%u bytes",
        static_cast<unsigned int>(
            json_configuration_length
        )
    );

    set_ble_status("OK:JSON_CHUNK");

    return true;
}


static void json_configuration_worker_task(
    void *parameter
)
{
    (void)parameter;

    ESP_LOGI(TAG, "JSON configuration worker started");

    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        const size_t json_length = json_worker_length;

        bool configuration_applied = false;

        if (
            json_configuration_handler != nullptr &&
            json_length > 0
        )
        {
            ESP_LOGI(
                TAG,
                "Worker applying JSON, length=%u",
                static_cast<unsigned int>(json_length)
            );

            configuration_applied =
                json_configuration_handler(
                    json_worker_buffer,
                    json_length
                );
        }

        if (configuration_applied)
        {
            set_ble_status("OK:JSON_APPLIED");
            ESP_LOGI(TAG, "JSON configuration applied");
        }
        else
        {
            set_ble_status("ERROR:JSON_PARSE_FAILED");
            ESP_LOGE(TAG, "JSON configuration failed");
        }

        taskENTER_CRITICAL(&json_worker_lock);
        json_worker_length = 0;
        json_worker_buffer[0] = '\0';
        json_worker_busy = false;
        taskEXIT_CRITICAL(&json_worker_lock);
    }
}


static bool finish_json_reception(void)
{
    if (!json_reception_active)
    {
        set_ble_status("ERROR:JSON_BEGIN_REQUIRED");
        return false;
    }

    json_reception_active = false;

    if (json_configuration_length == 0)
    {
        set_ble_status("ERROR:EMPTY_JSON");
        reset_json_reception();
        return false;
    }

    if (json_configuration_handler == nullptr)
    {
        set_ble_status("ERROR:NO_JSON_HANDLER");
        reset_json_reception();
        return false;
    }

    if (json_worker_task_handle == nullptr)
    {
        set_ble_status("ERROR:JSON_WORKER_NOT_READY");
        reset_json_reception();
        return false;
    }

    ESP_LOGI(
        TAG,
        "Complete JSON received, length=%u",
        static_cast<unsigned int>(json_configuration_length)
    );

    bool accepted = false;

    taskENTER_CRITICAL(&json_worker_lock);

    if (!json_worker_busy)
    {
        json_worker_busy = true;
        accepted = true;
    }

    taskEXIT_CRITICAL(&json_worker_lock);

    if (accepted)
    {
        /*
         * The worker cannot access this buffer until it is notified below,
         * so the larger copy is deliberately performed outside the critical
         * section.
         */
        memcpy(
            json_worker_buffer,
            json_configuration_buffer,
            json_configuration_length + 1
        );

        json_worker_length = json_configuration_length;
    }

    reset_json_reception();

    if (!accepted)
    {
        set_ble_status("ERROR:JSON_WORKER_BUSY");
        return false;
    }

    set_ble_status("OK:JSON_QUEUED");
    xTaskNotifyGive(json_worker_task_handle);

    ESP_LOGI(TAG, "JSON handed to configuration worker");

    return true;
}


/* =========================================================
 * BLE command processor
 * ========================================================= */

static bool process_ble_command(
    const char *command
)
{
    if (
        command == nullptr ||
        command[0] == '\0'
    )
    {
        set_ble_status(
            "ERROR:EMPTY_COMMAND"
        );

        return false;
    }

    /*
     * Device identity commands:
     *
     * DEVICE:GET
     * DEVICE:NAME:GET
     * DEVICE:NAME:SET:<new name>
     * DEVICE:MAC:GET
     */
    if (strcmp(command, "DEVICE:NAME:GET") == 0)
    {
        char response[64] = {};

        snprintf(
            response,
            sizeof(response),
            "DEVICE_NAME:%s",
            current_ble_device_name
        );

        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "MAC:GET") == 0)
    {
        char mac_text[18] = {};

        if (
            !get_bluetooth_mac_string(
                mac_text,
                sizeof(mac_text)
            )
        )
        {
            set_ble_status("ERROR:DEVICE_MAC");
            return false;
        }

        char response[40] = {};

        snprintf(
            response,
            sizeof(response),
            "DEVICE_MAC:%s",
            mac_text
        );

        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "DEVICE:GET") == 0)
    {
        char mac_text[18] = {};

        if (
            !get_bluetooth_mac_string(
                mac_text,
                sizeof(mac_text)
            )
        )
        {
            set_ble_status("ERROR:DEVICE_MAC");
            return false;
        }

        char response[96] = {};

        snprintf(
            response,
            sizeof(response),
            "DEVICE:name=%s,mac=%s",
            current_ble_device_name,
            mac_text
        );

        set_ble_status(response);
        return true;
    }

    static constexpr char device_name_set_prefix[] =
        "DEVICE:NAME:SET:";

    static constexpr size_t device_name_set_prefix_length =
        sizeof(device_name_set_prefix) - 1;

    if (
        strncmp(
            command,
            device_name_set_prefix,
            device_name_set_prefix_length
        ) == 0
    )
    {
        const char *new_name =
            command +
            device_name_set_prefix_length;

        if (!is_valid_device_name(new_name))
        {
            set_ble_status(
                "ERROR:DEVICE_NAME_INVALID"
            );

            return false;
        }

        if (!save_ble_device_name(new_name))
        {
            set_ble_status(
                "ERROR:DEVICE_NAME_SAVE"
            );

            return false;
        }

        char response[64] = {};

        snprintf(
            response,
            sizeof(response),
            "OK:DEVICE_NAME_SET:%s",
            current_ble_device_name
        );

        set_ble_status(response);
        return true;
    }


    if (strncmp(command, "WIFI:SET:", 9) == 0)
    {
        char credentials[100] = {};
        std::strncpy(credentials, command + 9, sizeof(credentials) - 1);

        char* separator = std::strchr(credentials, '|');
        if (separator == nullptr)
        {
            set_ble_status("ERROR:WIFI_FORMAT");
            return false;
        }

        *separator = '\0';
        const char* ssid = credentials;
        const char* password = separator + 1;

        if (!wifi_manager_set_credentials(ssid, password))
        {
            set_ble_status("ERROR:WIFI_SAVE");
            return false;
        }

        set_ble_status("OK:WIFI_SAVED");
        return true;
    }

    if (strcmp(command, "WIFI:STATUS") == 0)
    {
        char response[128] = {};
        wifi_manager_get_status(response, sizeof(response));
        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "OTA:INFO") == 0)
    {
        char response[128] = {};
        std::snprintf(
            response,
            sizeof(response),
            "OTA_INFO:version=%s,busy=%s",
            FROST_FIRMWARE_VERSION,
            ota_manager_is_busy() ? "yes" : "no"
        );
        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "OTA:CHECK") == 0)
    {
        const bool queued = ota_manager_request_check();
        set_ble_status(queued ? "OK:OTA_CHECK_QUEUED" : "ERROR:OTA_BUSY");
        return queued;
    }

    if (strcmp(command, "OTA:START") == 0)
    {
        const bool queued = ota_manager_request_start();
        set_ble_status(queued ? "OK:OTA_START_QUEUED" : "ERROR:OTA_BUSY");
        return queued;
    }

    if (strcmp(command, "OTA:STATUS") == 0)
    {
        char response[128] = {};
        ota_manager_get_status(response, sizeof(response));
        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "OTA:CANCEL") == 0)
    {
        const bool accepted = ota_manager_request_cancel();
        set_ble_status(accepted ? "OK:OTA_CANCEL_REQUESTED" : "ERROR:OTA_NOT_ACTIVE");
        return accepted;
    }

    /*
     * RTC command:
     *
     * SET 2026-07-20 22:30:00
     */
    if (strncmp(command, "SET ", 4) == 0)
    {
        bool success =
            rtc_ds3231_process_set_command(
                command
            );

        set_ble_status(
            success
                ? "OK:TIME_SET"
                : "ERROR:TIME_SET_FAILED"
        );

        return success;
    }

    if (strcmp(command, "JSON_BEGIN") == 0)
    {
        begin_json_reception();
        return true;
    }

    static constexpr char json_chunk_prefix[] =
        "JSON_CHUNK:";

    static constexpr size_t json_chunk_prefix_length =
        sizeof(json_chunk_prefix) - 1;

    if (
        strncmp(
            command,
            json_chunk_prefix,
            json_chunk_prefix_length
        ) == 0
    )
    {
        return append_json_chunk(
            command +
            json_chunk_prefix_length
        );
    }

    if (strcmp(command, "JSON_END") == 0)
    {
        return finish_json_reception();
    }

    if (strcmp(command, "JSON_CANCEL") == 0)
    {
        reset_json_reception();

        set_ble_status(
            "OK:JSON_CANCELLED"
        );

        return true;
    }



    if (
        strcmp(command, "STATS:GET") == 0 ||
        strcmp(command, "STATS:TODAY") == 0
    )
    {
        statistics_line_cursor = 0;
        statistics_export_mode = StatisticsExportMode::TODAY;

        char response[128] = {};
        if (!user_statistics_get_line(
                statistics_line_cursor,
                response,
                sizeof(response)
            ))
        {
            set_ble_status("ERROR:STATS_NOT_READY");
            return false;
        }

        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "STATS:HISTORY") == 0)
    {
        statistics_line_cursor = 0;
        statistics_export_mode = StatisticsExportMode::HISTORY;

        char response[128] = {};
        if (!statistics_history_get_line(0, response, sizeof(response)))
        {
            set_ble_status("ERROR:HISTORY_NOT_READY");
            return false;
        }
        set_ble_status(response);
        return true;
    }

    static constexpr char stats_day_prefix[] = "STATS:DAY:";
    if (strncmp(command, stats_day_prefix, sizeof(stats_day_prefix) - 1) == 0)
    {
        const char* date_text = command + sizeof(stats_day_prefix) - 1;
        char* end = nullptr;
        const unsigned long day = std::strtoul(date_text, &end, 10);
        if (end == date_text || *end != '\0' || day > 99999999UL ||
            !statistics_history_select_day(static_cast<uint32_t>(day)))
        {
            set_ble_status("ERROR:HISTORY_DAY_NOT_FOUND");
            return false;
        }

        statistics_line_cursor = 0;
        statistics_export_mode = StatisticsExportMode::SELECTED_DAY;
        char response[128] = {};
        if (!statistics_history_get_selected_day_line(0, response, sizeof(response)))
        {
            set_ble_status("ERROR:HISTORY_DAY");
            return false;
        }
        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "STATS:HISTORY_CLEAR") == 0)
    {
        if (!statistics_history_clear())
        {
            set_ble_status("ERROR:HISTORY_CLEAR");
            return false;
        }
        statistics_line_cursor = 0;
        statistics_export_mode = StatisticsExportMode::TODAY;
        set_ble_status("HISTORY_CLEAR_OK");
        return true;
    }

    if (strcmp(command, "STATS:NEXT") == 0 ||
        strcmp(command, "STATS:HISTORY_NEXT") == 0)
    {
        std::size_t line_count = 0;
        if (statistics_export_mode == StatisticsExportMode::TODAY)
            line_count = user_statistics_line_count();
        else if (statistics_export_mode == StatisticsExportMode::HISTORY)
            line_count = statistics_history_line_count();
        else
            line_count = statistics_history_selected_day_line_count();

        if (line_count == 0)
        {
            set_ble_status("ERROR:STATS_NOT_READY");
            return false;
        }

        if (statistics_line_cursor + 1 < line_count)
        {
            ++statistics_line_cursor;
        }

        char response[128] = {};
        bool ok = false;
        if (statistics_export_mode == StatisticsExportMode::TODAY)
            ok = user_statistics_get_line(statistics_line_cursor, response, sizeof(response));
        else if (statistics_export_mode == StatisticsExportMode::HISTORY)
            ok = statistics_history_get_line(statistics_line_cursor, response, sizeof(response));
        else
            ok = statistics_history_get_selected_day_line(statistics_line_cursor, response, sizeof(response));

        if (!ok)
        {
            set_ble_status("ERROR:STATS_LINE");
            return false;
        }

        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "STATS:RESET") == 0)
    {
        user_statistics_reset_today();
        statistics_line_cursor = 0;
        statistics_export_mode = StatisticsExportMode::TODAY;
        set_ble_status("STATS_RESET_OK");
        return true;
    }

    if (strcmp(command, "BOTTLE:LEARN_START") == 0)
    {
        bool expected = false;

        if (!bottle_calibration_start_requested.compare_exchange_strong(
                expected,
                true
            ))
        {
            set_ble_status("ERROR:BOTTLE_LEARN_ALREADY_QUEUED");
            return true;
        }

        set_ble_status("OK:BOTTLE_LEARN_QUEUED");
        ESP_LOGI(TAG, "Bottle calibration start request queued");
        return true;
    }

    if (strcmp(command, "BOTTLE:LEARN_CANCEL") == 0)
    {
        bottle_calibration_cancel_requested.store(true);
        set_ble_status("OK:BOTTLE_LEARN_CANCEL_QUEUED");
        ESP_LOGI(TAG, "Bottle calibration cancel request queued");
        return true;
    }

    if (strcmp(command, "BOTTLE:STATUS") == 0)
    {
        char response[128] = {};
        bottle_calibration_get_status(response, sizeof(response));
        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "POMO:LAP_STATUS") == 0)
    {
        char response[128] = {};

        const int active_index =
            pomodoro_get_active_lap_index();

        std::snprintf(
            response,
            sizeof(response),
            "POMO_LAP:mode=%s,active=%s,index=%d,laps=%u",
            pomodoro_is_lap_mode_enabled()
                ? "lap"
                : "manual",
            pomodoro_is_running()
                ? "running"
                : "idle",
            active_index,
            static_cast<unsigned>(
                pomodoro_get_lap_count()
            )
        );

        set_ble_status(response);
        return true;
    }

    if (strcmp(command, "GET_TIME") == 0)
    {
        char time_text[40];

        if (
            rtc_ds3231_get_time_string(
                time_text,
                sizeof(time_text)
            )
        )
        {
            char response[64];

            int written = snprintf(
                response,
                sizeof(response),
                "TIME:%s",
                time_text
            );

            if (
                written < 0 ||
                static_cast<size_t>(written) >=
                    sizeof(response)
            )
            {
                set_ble_status(
                    "ERROR:TIME_RESPONSE"
                );

                return false;
            }

            set_ble_status(response);
            return true;
        }

        set_ble_status(
            "ERROR:RTC_READ_FAILED"
        );

        return false;
    }

    set_ble_status(
        "ERROR:UNKNOWN_COMMAND"
    );

    ESP_LOGE(
        TAG,
        "Unknown command: %s",
        command
    );

    return false;
}


/* =========================================================
 * GATT characteristic
 * ========================================================= */

static int command_characteristic_access(
    uint16_t connection_handle,
    uint16_t attribute_handle,
    struct ble_gatt_access_ctxt *context,
    void *argument
)
{
    (void)connection_handle;
    (void)attribute_handle;
    (void)argument;

    if (
        context->op ==
        BLE_GATT_ACCESS_OP_WRITE_CHR
    )
    {
        uint16_t packet_length =
            OS_MBUF_PKTLEN(context->om);

        if (
            packet_length == 0 ||
            packet_length >=
                BLE_COMMAND_MAX_LEN
        )
        {
            ESP_LOGE(
                TAG,
                "Invalid command length: %u",
                packet_length
            );

            set_ble_status(
                "ERROR:COMMAND_TOO_LONG"
            );

            return
                BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        char command[BLE_COMMAND_MAX_LEN];

        uint16_t copied_length = 0;

        int result = ble_hs_mbuf_to_flat(
            context->om,
            command,
            sizeof(command) - 1,
            &copied_length
        );

        if (result != 0)
        {
            ESP_LOGE(
                TAG,
                "Could not read BLE packet, rc=%d",
                result
            );

            return BLE_ATT_ERR_UNLIKELY;
        }

        command[copied_length] = '\0';

        /*
         * Remove CR/LF from terminal applications.
         */
        while (
            copied_length > 0 &&
            (
                command[copied_length - 1] == '\r' ||
                command[copied_length - 1] == '\n'
            )
        )
        {
            copied_length--;
            command[copied_length] = '\0';
        }

        ESP_LOGI(
            TAG,
            "Command received: %s",
            command
        );

        /*
         * The BLE write itself succeeded. Application-level failures are
         * reported through last_ble_status as ERROR:...
         */
        process_ble_command(command);
        return 0;
    }

    if (
        context->op ==
        BLE_GATT_ACCESS_OP_READ_CHR
    )
    {
        int result = os_mbuf_append(
            context->om,
            last_ble_status,
            strlen(last_ble_status)
        );

        if (result != 0)
        {
            return
                BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}


/* =========================================================
 * GATT services
 * =========================================================
 *
 * ESP-IDF 6 / GCC 15 treats missing and out-of-order C++
 * designated initializers as errors. Build these structures
 * at runtime instead of using designated initializers.
 */

static struct ble_gatt_chr_def
gatt_characteristics[2] = {};

static struct ble_gatt_svc_def
gatt_services[2] = {};


static void initialize_gatt_definitions(void)
{
    memset(
        gatt_characteristics,
        0,
        sizeof(gatt_characteristics)
    );

    memset(
        gatt_services,
        0,
        sizeof(gatt_services)
    );

    gatt_characteristics[0].uuid =
        &frost_command_uuid.u;

    gatt_characteristics[0].access_cb =
        command_characteristic_access;

    gatt_characteristics[0].arg =
        nullptr;

    gatt_characteristics[0].descriptors =
        nullptr;

    gatt_characteristics[0].flags =
        BLE_GATT_CHR_F_READ |
        BLE_GATT_CHR_F_WRITE |
        BLE_GATT_CHR_F_WRITE_NO_RSP;

    gatt_characteristics[0].min_key_size =
        0;

    gatt_characteristics[0].val_handle =
        &command_value_handle;

    gatt_characteristics[0].cpfd =
        nullptr;

    /*
     * gatt_characteristics[1] remains zeroed and terminates
     * the characteristic array.
     */

    gatt_services[0].type =
        BLE_GATT_SVC_TYPE_PRIMARY;

    gatt_services[0].uuid =
        &frost_service_uuid.u;

    gatt_services[0].includes =
        nullptr;

    gatt_services[0].characteristics =
        gatt_characteristics;

    /*
     * gatt_services[1] remains zeroed and terminates
     * the service array.
     */
}


/* =========================================================
 * Advertising
 * ========================================================= */

static void start_advertising(void);


static int gap_event_handler(
    struct ble_gap_event *event,
    void *argument
)
{
    (void)argument;

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:

            if (event->connect.status == 0)
            {
                ESP_LOGI(
                    TAG,
                    "BLE connected, handle=%u",
                    event->connect.conn_handle
                );

                set_ble_status(
                    "OK:CONNECTED"
                );
            }
            else
            {
                ESP_LOGW(
                    TAG,
                    "Connection failed, status=%d",
                    event->connect.status
                );

                start_advertising();
            }

            return 0;

        case BLE_GAP_EVENT_DISCONNECT:

            ESP_LOGI(
                TAG,
                "BLE disconnected, reason=%d",
                event->disconnect.reason
            );

            reset_json_reception();

            start_advertising();

            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:

            start_advertising();
            return 0;

        case BLE_GAP_EVENT_MTU:

            ESP_LOGI(
                TAG,
                "MTU updated: %u",
                event->mtu.value
            );

            return 0;

        default:
            return 0;
    }
}


static void start_advertising(void)
{
    int result = ble_gap_adv_stop();

    if (
        result != 0 &&
        result != BLE_HS_EALREADY &&
        result != BLE_HS_ENOTCONN
    )
    {
        ESP_LOGW(
            TAG,
            "ble_gap_adv_stop returned %d",
            result
        );
    }

    struct ble_hs_adv_fields fields;

    memset(
        &fields,
        0,
        sizeof(fields)
    );

    fields.flags =
        BLE_HS_ADV_F_DISC_GEN |
        BLE_HS_ADV_F_BREDR_UNSUP;

    const char *device_name =
        ble_svc_gap_device_name();

    fields.name =
        reinterpret_cast<const uint8_t *>(
            device_name
        );

    fields.name_len =
        strlen(device_name);

    fields.name_is_complete = 1;

    result = ble_gap_adv_set_fields(
        &fields
    );

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "Advertising fields failed, rc=%d",
            result
        );

        return;
    }

    struct ble_hs_adv_fields response_fields;

    memset(
        &response_fields,
        0,
        sizeof(response_fields)
    );

    response_fields.uuids128 =
        const_cast<ble_uuid128_t *>(
            &frost_service_uuid
        );

    response_fields.num_uuids128 = 1;
    response_fields.uuids128_is_complete = 1;

    result = ble_gap_adv_rsp_set_fields(
        &response_fields
    );

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "Scan response failed, rc=%d",
            result
        );

        return;
    }

    struct ble_gap_adv_params parameters;

    memset(
        &parameters,
        0,
        sizeof(parameters)
    );

    parameters.conn_mode =
        BLE_GAP_CONN_MODE_UND;

    parameters.disc_mode =
        BLE_GAP_DISC_MODE_GEN;

    result = ble_gap_adv_start(
        own_address_type,
        nullptr,
        BLE_HS_FOREVER,
        &parameters,
        gap_event_handler,
        nullptr
    );

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "Advertising start failed, rc=%d",
            result
        );

        return;
    }

    ESP_LOGI(
        TAG,
        "Advertising as %s",
        device_name
    );
}


/* =========================================================
 * NimBLE lifecycle
 * ========================================================= */

static void nimble_on_reset(int reason)
{
    ESP_LOGE(
        TAG,
        "NimBLE reset, reason=%d",
        reason
    );
}


static void nimble_on_sync(void)
{
    int result =
        ble_hs_util_ensure_addr(0);

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "No BLE address, rc=%d",
            result
        );

        return;
    }

    result = ble_hs_id_infer_auto(
        0,
        &own_address_type
    );

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "Address inference failed, rc=%d",
            result
        );

        return;
    }

    start_advertising();
}


static void nimble_host_task(
    void *parameter
)
{
    (void)parameter;

    ESP_LOGI(
        TAG,
        "NimBLE host task started"
    );

    nimble_port_run();

    nimble_port_freertos_deinit();
}


/* =========================================================
 * Public initialization
 * ========================================================= */

esp_err_t bluetooth_init(
    bluetooth_json_handler_t json_handler
)
{
    if (ble_initialized)
    {
        return ESP_OK;
    }

    if (json_handler == nullptr)
    {
        ESP_LOGE(
            TAG,
            "JSON handler cannot be null"
        );

        return ESP_ERR_INVALID_ARG;
    }

    json_configuration_handler =
        json_handler;

    reset_json_reception();

    /*
     * NVS is initialized by app_main() before bluetooth_init().
     */
    load_ble_device_name();

    esp_err_t error =
        nimble_port_init();

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "nimble_port_init failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    ble_hs_cfg.reset_cb =
        nimble_on_reset;

    ble_hs_cfg.sync_cb =
        nimble_on_sync;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    initialize_gatt_definitions();

    int result =
        ble_svc_gap_device_name_set(
            current_ble_device_name
        );

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "Could not set BLE name, rc=%d",
            result
        );

        return ESP_FAIL;
    }

    result = ble_gatts_count_cfg(
        gatt_services
    );

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "ble_gatts_count_cfg failed, rc=%d",
            result
        );

        return ESP_FAIL;
    }

    result = ble_gatts_add_svcs(
        gatt_services
    );

    if (result != 0)
    {
        ESP_LOGE(
            TAG,
            "ble_gatts_add_svcs failed, rc=%d",
            result
        );

        return ESP_FAIL;
    }

    BaseType_t task_result = xTaskCreate(
        json_configuration_worker_task,
        "ble_json_worker",
        JSON_WORKER_STACK_SIZE,
        nullptr,
        JSON_WORKER_PRIORITY,
        &json_worker_task_handle
    );

    if (task_result != pdPASS)
    {
        ESP_LOGE(TAG, "Could not create JSON worker task");
        json_worker_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(
        nimble_host_task
    );

    ble_initialized = true;

    ESP_LOGI(
        TAG,
        "Bluetooth initialized"
    );

    return ESP_OK;
}


bool bluetooth_is_initialized(void)
{
    return ble_initialized;
}

bool bluetooth_take_bottle_calibration_start_request(void)
{
    return bottle_calibration_start_requested.exchange(false);
}

bool bluetooth_take_bottle_calibration_cancel_request(void)
{
    return bottle_calibration_cancel_requested.exchange(false);
}

const char *bluetooth_get_device_name(void)
{
    return current_ble_device_name;
}