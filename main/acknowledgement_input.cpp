#include "acknowledgement_input.hpp"

#include <cstdint>

#include "esp_log.h"
#include "esp_timer.h"

static const char* TAG = "ACK_INPUT";

static AcknowledgementInputConfig input_config;
static acknowledgement_callback_t acknowledgement_callback = nullptr;

static bool initialized = false;

/*
 * Raw pin level and debounced stable level.
 */
static int raw_level = 1;
static int stable_level = 1;

static uint64_t raw_changed_ms = 0;

static uint64_t current_millis()
{
    return static_cast<uint64_t>(
        esp_timer_get_time() / 1000ULL
    );
}

static bool is_asserted(int level)
{
    return input_config.active_low
        ? level == 0
        : level != 0;
}

esp_err_t acknowledgement_input_init(
    const AcknowledgementInputConfig& config,
    acknowledgement_callback_t callback
)
{
    if (callback == nullptr)
    {
        ESP_LOGE(TAG, "Acknowledgement callback is null");
        return ESP_ERR_INVALID_ARG;
    }

    input_config = config;
    acknowledgement_callback = callback;

    gpio_config_t gpio_configuration = {};

    gpio_configuration.pin_bit_mask =
        1ULL << static_cast<unsigned>(input_config.gpio);

    gpio_configuration.mode = GPIO_MODE_INPUT;

    /*
     * Typical IR modules have a driven digital output.
     * The pull-up also prevents a floating input during startup.
     */
    gpio_configuration.pull_up_en =
        input_config.active_low
            ? GPIO_PULLUP_ENABLE
            : GPIO_PULLUP_DISABLE;

    gpio_configuration.pull_down_en =
        input_config.active_low
            ? GPIO_PULLDOWN_DISABLE
            : GPIO_PULLDOWN_ENABLE;

    gpio_configuration.intr_type = GPIO_INTR_DISABLE;

    const esp_err_t error =
        gpio_config(&gpio_configuration);

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "GPIO configuration failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    raw_level = gpio_get_level(input_config.gpio);
    stable_level = raw_level;
    raw_changed_ms = current_millis();

    initialized = true;

    ESP_LOGI(
        TAG,
        "Acknowledgement input ready on GPIO %d, active_%s",
        static_cast<int>(input_config.gpio),
        input_config.active_low ? "low" : "high"
    );

    return ESP_OK;
}

void acknowledgement_input_update()
{
    if (!initialized)
    {
        return;
    }

    const uint64_t now_ms = current_millis();
    const int new_raw_level =
        gpio_get_level(input_config.gpio);

    if (new_raw_level != raw_level)
    {
        raw_level = new_raw_level;
        raw_changed_ms = now_ms;
    }

    if (
        raw_level == stable_level ||
        now_ms - raw_changed_ms <
            static_cast<uint64_t>(input_config.debounce_ms)
    )
    {
        return;
    }

    const bool was_asserted =
        is_asserted(stable_level);

    stable_level = raw_level;

    const bool now_asserted =
        is_asserted(stable_level);

    /*
     * Trigger only once on a new inactive -> active transition.
     */
    if (!was_asserted && now_asserted)
    {
        ESP_LOGI(TAG, "Acknowledgement gesture detected");
        acknowledgement_callback();
    }
}

bool acknowledgement_input_is_initialized()
{
    return initialized;
}