#include "dfplayer.hpp"

#include <algorithm>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace
{
constexpr uart_port_t UART_PORT = UART_NUM_1;
constexpr gpio_num_t TX_PIN = GPIO_NUM_17;
constexpr gpio_num_t RX_PIN = GPIO_NUM_18;
constexpr gpio_num_t BUSY_PIN = GPIO_NUM_14;
constexpr int BAUD_RATE = 9600;
constexpr int RX_BUFFER_SIZE = 512;
constexpr uint8_t FEEDBACK = 0x00;

constexpr uint8_t CMD_PLAY_TRACK = 0x03;
constexpr uint8_t CMD_SET_VOLUME = 0x06;
constexpr uint8_t CMD_SELECT_DEVICE = 0x09;
constexpr uint8_t CMD_RESUME = 0x0D;
constexpr uint8_t CMD_PAUSE = 0x0E;
constexpr uint8_t CMD_ADVERTISE = 0x13;
constexpr uint8_t CMD_STOP = 0x16;

constexpr uint16_t DEVICE_TF_CARD = 0x0002;

const char* TAG = "DFPLAYER";
bool initialized = false;

uint16_t checksum(
    uint8_t command,
    uint8_t feedback,
    uint16_t parameter)
{
    const uint16_t sum =
        0xFFU +
        0x06U +
        command +
        feedback +
        ((parameter >> 8U) & 0xFFU) +
        (parameter & 0xFFU);

    return static_cast<uint16_t>(0U - sum);
}

esp_err_t send_command(
    uint8_t command,
    uint16_t parameter)
{
    if (!initialized && command != CMD_SELECT_DEVICE)
    {
        ESP_LOGW(TAG, "Command 0x%02X sent before initialization", command);
    }

    const uint16_t value =
        checksum(command, FEEDBACK, parameter);

    const uint8_t frame[10] = {
        0x7E,
        0xFF,
        0x06,
        command,
        FEEDBACK,
        static_cast<uint8_t>((parameter >> 8U) & 0xFFU),
        static_cast<uint8_t>(parameter & 0xFFU),
        static_cast<uint8_t>((value >> 8U) & 0xFFU),
        static_cast<uint8_t>(value & 0xFFU),
        0xEF
    };

    const int written =
        uart_write_bytes(
            UART_PORT,
            reinterpret_cast<const char*>(frame),
            sizeof(frame)
        );

    if (written != static_cast<int>(sizeof(frame)))
    {
        ESP_LOGE(
            TAG,
            "UART write failed: wrote %d of %u bytes",
            written,
            static_cast<unsigned>(sizeof(frame))
        );

        return ESP_FAIL;
    }

    const esp_err_t result =
        uart_wait_tx_done(
            UART_PORT,
            pdMS_TO_TICKS(200)
        );

    if (result == ESP_OK)
    {
        ESP_LOGI(
            TAG,
            "Command=0x%02X parameter=%u",
            command,
            static_cast<unsigned>(parameter)
        );
    }

    return result;
}
}

esp_err_t dfplayer_init(uint8_t volume)
{
    if (initialized)
    {
        return dfplayer_set_volume(volume);
    }

    const uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {}
    };

    esp_err_t result =
        uart_driver_install(
            UART_PORT,
            RX_BUFFER_SIZE,
            0,
            0,
            nullptr,
            0
        );

    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(result));
        return result;
    }

    ESP_RETURN_ON_ERROR(
        uart_param_config(UART_PORT, &uart_config),
        TAG,
        "UART parameter configuration failed"
    );

    ESP_RETURN_ON_ERROR(
        uart_set_pin(
            UART_PORT,
            TX_PIN,
            RX_PIN,
            UART_PIN_NO_CHANGE,
            UART_PIN_NO_CHANGE
        ),
        TAG,
        "UART pin configuration failed"
    );

    const gpio_config_t busy_config = {
        .pin_bit_mask = 1ULL << BUSY_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_RETURN_ON_ERROR(
        gpio_config(&busy_config),
        TAG,
        "BUSY pin configuration failed"
    );

    ESP_LOGI(TAG, "Waiting for DFPlayer and TF card...");
    vTaskDelay(pdMS_TO_TICKS(4000));

    initialized = true;

    ESP_RETURN_ON_ERROR(
        send_command(CMD_SELECT_DEVICE, DEVICE_TF_CARD),
        TAG,
        "TF card selection failed"
    );

    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_RETURN_ON_ERROR(
        dfplayer_set_volume(volume),
        TAG,
        "Volume configuration failed"
    );

    ESP_LOGI(
        TAG,
        "Ready: TX=%d RX=%d BUSY=%d volume=%u",
        static_cast<int>(TX_PIN),
        static_cast<int>(RX_PIN),
        static_cast<int>(BUSY_PIN),
        static_cast<unsigned>(std::min<uint8_t>(volume, 30))
    );

    return ESP_OK;
}

esp_err_t dfplayer_set_volume(uint8_t volume)
{
    return send_command(
        CMD_SET_VOLUME,
        std::min<uint8_t>(volume, 30)
    );
}

esp_err_t dfplayer_play_track(uint16_t track)
{
    if (track == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return send_command(CMD_PLAY_TRACK, track);
}

esp_err_t dfplayer_play_advertisement(uint16_t track)
{
    if (track == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    return send_command(CMD_ADVERTISE, track);
}

esp_err_t dfplayer_pause()
{
    return send_command(CMD_PAUSE, 0);
}

esp_err_t dfplayer_resume()
{
    return send_command(CMD_RESUME, 0);
}

esp_err_t dfplayer_stop()
{
    return send_command(CMD_STOP, 0);
}

bool dfplayer_is_playing()
{
    return gpio_get_level(BUSY_PIN) == 0;
}

void dfplayer_update()
{
    uint8_t data[64];

    const int length =
        uart_read_bytes(
            UART_PORT,
            data,
            sizeof(data),
            0
        );

    if (length > 0)
    {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, data, length, ESP_LOG_DEBUG);
    }
}