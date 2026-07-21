#include "bluetooth.hpp"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
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


static const char *TAG = "FROST_BLE";


/* =========================================================
 * BLE configuration
 * ========================================================= */

#define BLE_DEVICE_NAME       "ESP32_RTC"
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

        if (!process_ble_command(command))
        {
            /*
             * The status text contains the detailed reason.
             */
            return
                BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }

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
        BLE_DEVICE_NAME
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
            BLE_DEVICE_NAME
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