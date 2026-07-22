#include "hx711.hpp"

#include "esp_log.h"
#include "esp_rom_sys.h"

namespace
{
const char* TAG = "HX711";

Hx711Config hx_config = {};
bool initialized = false;

bool wait_until_ready()
{
    uint32_t waited_us = 0;

    while (gpio_get_level(hx_config.dout_gpio) != 0)
    {
        if (waited_us >= hx_config.ready_timeout_us)
        {
            return false;
        }

        esp_rom_delay_us(10);
        waited_us += 10;
    }

    return true;
}
}

esp_err_t hx711_init(const Hx711Config& config)
{
    hx_config = config;

    gpio_config_t dout = {};
    dout.pin_bit_mask = 1ULL << static_cast<unsigned>(hx_config.dout_gpio);
    dout.mode = GPIO_MODE_INPUT;
    dout.pull_up_en = GPIO_PULLUP_ENABLE;
    dout.pull_down_en = GPIO_PULLDOWN_DISABLE;
    dout.intr_type = GPIO_INTR_DISABLE;

    esp_err_t error = gpio_config(&dout);

    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "DOUT GPIO setup failed: %s", esp_err_to_name(error));
        return error;
    }

    gpio_config_t sck = {};
    sck.pin_bit_mask = 1ULL << static_cast<unsigned>(hx_config.sck_gpio);
    sck.mode = GPIO_MODE_OUTPUT;
    sck.pull_up_en = GPIO_PULLUP_DISABLE;
    sck.pull_down_en = GPIO_PULLDOWN_ENABLE;
    sck.intr_type = GPIO_INTR_DISABLE;

    error = gpio_config(&sck);

    if (error != ESP_OK)
    {
        ESP_LOGE(TAG, "SCK GPIO setup failed: %s", esp_err_to_name(error));
        return error;
    }

    gpio_set_level(hx_config.sck_gpio, 0);
    initialized = true;

    ESP_LOGI(
        TAG,
        "HX711 ready: DOUT=%d SCK=%d",
        static_cast<int>(hx_config.dout_gpio),
        static_cast<int>(hx_config.sck_gpio)
    );

    return ESP_OK;
}

bool hx711_is_initialized()
{
    return initialized;
}

bool hx711_is_ready()
{
    return initialized &&
           gpio_get_level(hx_config.dout_gpio) == 0;
}

bool hx711_read_raw(int32_t& raw_value)
{
    if (!initialized || !wait_until_ready())
    {
        return false;
    }

    uint32_t value = 0;

    /*
     * Keep the 24 clock pulses tight so SCK never remains HIGH long
     * enough to accidentally power down the HX711.
     */
    for (int bit = 0; bit < 24; ++bit)
    {
        gpio_set_level(hx_config.sck_gpio, 1);
        esp_rom_delay_us(1);

        value = (value << 1U) |
                static_cast<uint32_t>(
                    gpio_get_level(hx_config.dout_gpio) != 0
                );

        gpio_set_level(hx_config.sck_gpio, 0);
        esp_rom_delay_us(1);
    }

    /*
     * 25th pulse: channel A, gain 128 for the next conversion.
     */
    gpio_set_level(hx_config.sck_gpio, 1);
    esp_rom_delay_us(1);
    gpio_set_level(hx_config.sck_gpio, 0);
    esp_rom_delay_us(1);

    if ((value & 0x00800000U) != 0)
    {
        value |= 0xFF000000U;
    }

    raw_value = static_cast<int32_t>(value);
    return true;
}

void hx711_power_down()
{
    if (!initialized)
    {
        return;
    }

    gpio_set_level(hx_config.sck_gpio, 0);
    gpio_set_level(hx_config.sck_gpio, 1);
    esp_rom_delay_us(70);
}

void hx711_power_up()
{
    if (!initialized)
    {
        return;
    }

    gpio_set_level(hx_config.sck_gpio, 0);
}