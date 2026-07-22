#include "consumption_tracker.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

#include "display.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "scale.hpp"

namespace
{
const char* TAG = "CONSUMPTION";

/* Mandatory physical settling period requested for bottle return. */
constexpr uint32_t BOTTLE_SETTLING_DELAY_MS = 5000;

/* Separate hydration-consumption screen remains visible for 5 seconds. */
constexpr uint32_t CONSUMPTION_SCREEN_DURATION_MS = 5000;

/* Same Arduino classification thresholds. */
constexpr float MIN_CONSUMPTION_ML = 10.0f;
constexpr float REFILL_THRESHOLD_ML = -20.0f;
constexpr float CAPACITY_NOISE_ALLOWANCE_ML = 20.0f;

/* Non-blocking stable reading window. */
constexpr uint32_t SAMPLE_INTERVAL_MS = 80;
constexpr std::size_t SAMPLE_COUNT = 20;
constexpr float MAX_TRIMMED_RANGE_G = 8.0f;
constexpr float MAX_MEAN_DRIFT_G = 2.5f;
constexpr uint32_t REQUIRED_STABLE_MS = 1400;
constexpr uint32_t MEASUREMENT_TIMEOUT_MS = 15000;

constexpr const char* NVS_NAMESPACE = "hydration";
constexpr const char* NVS_KEY = "daily";
constexpr uint32_t STORED_VERSION = 1;
constexpr uint32_t DEFAULT_DAILY_GOAL_ML = 3000;

struct StoredDaily
{
    uint32_t version;
    uint32_t yyyymmdd;
    uint32_t consumed_ml;
    uint32_t goal_ml;
};

enum class TrackerState : uint8_t
{
    IDLE,
    SETTLING,
    MEASURING
};

TrackerState state = TrackerState::IDLE;
bool initialized = false;
bool tracking_enabled = true;
bool dock_state_known = false;
bool docked = false;

uint64_t settling_started_ms = 0;
uint64_t measuring_started_ms = 0;
uint64_t last_sample_ms = 0;
uint64_t stable_since_ms = 0;

float samples[SAMPLE_COUNT] = {};
std::size_t sample_count = 0;
std::size_t sample_index = 0;
float previous_mean = 0.0f;
bool previous_mean_valid = false;

float last_remaining_ml = 0.0f;
uint32_t last_consumed_ml = 0;
uint32_t daily_consumed_ml = 0;
uint32_t daily_goal_ml = DEFAULT_DAILY_GOAL_ML;

bool screen_active = false;
uint64_t screen_started_ms = 0;

uint64_t now_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

uint32_t current_yyyymmdd()
{
    const time_t now = time(nullptr);
    struct tm value = {};
    localtime_r(&now, &value);

    const int year = value.tm_year + 1900;

    if (year < 2025)
    {
        return 0;
    }

    return static_cast<uint32_t>(
        year * 10000 +
        (value.tm_mon + 1) * 100 +
        value.tm_mday
    );
}

void reset_measurement_window()
{
    std::memset(samples, 0, sizeof(samples));
    sample_count = 0;
    sample_index = 0;
    last_sample_ms = 0;
    stable_since_ms = 0;
    previous_mean = 0.0f;
    previous_mean_valid = false;
}

void save_daily()
{
    StoredDaily stored = {};
    stored.version = STORED_VERSION;
    stored.yyyymmdd = current_yyyymmdd();
    stored.consumed_ml = daily_consumed_ml;
    stored.goal_ml = daily_goal_ml;

    nvs_handle_t handle = 0;

    esp_err_t error = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle
    );

    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "Unable to open hydration NVS: %s", esp_err_to_name(error));
        return;
    }

    error = nvs_set_blob(handle, NVS_KEY, &stored, sizeof(stored));

    if (error == ESP_OK)
    {
        error = nvs_commit(handle);
    }

    nvs_close(handle);

    if (error != ESP_OK)
    {
        ESP_LOGW(TAG, "Unable to save hydration data: %s", esp_err_to_name(error));
    }
}

void load_daily()
{
    nvs_handle_t handle = 0;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK)
    {
        return;
    }

    StoredDaily stored = {};
    std::size_t size = sizeof(stored);

    const esp_err_t error = nvs_get_blob(
        handle,
        NVS_KEY,
        &stored,
        &size
    );

    nvs_close(handle);

    if (
        error != ESP_OK ||
        size != sizeof(stored) ||
        stored.version != STORED_VERSION
    )
    {
        return;
    }

    if (stored.goal_ml > 0)
    {
        daily_goal_ml = stored.goal_ml;
    }

    const uint32_t today = current_yyyymmdd();

    if (today != 0 && stored.yyyymmdd == today)
    {
        daily_consumed_ml = stored.consumed_ml;
    }
    else
    {
        daily_consumed_ml = 0;
        save_daily();
    }
}

void reset_for_new_day_if_needed()
{
    static uint32_t active_day = 0;
    const uint32_t today = current_yyyymmdd();

    if (today == 0)
    {
        return;
    }

    if (active_day == 0)
    {
        active_day = today;
        return;
    }

    if (active_day != today)
    {
        active_day = today;
        daily_consumed_ml = 0;
        last_consumed_ml = 0;
        save_daily();
        ESP_LOGI(TAG, "Daily hydration total reset for new day");
    }
}

float trimmed_mean_and_range(float& range)
{
    float ordered[SAMPLE_COUNT] = {};

    for (std::size_t i = 0; i < sample_count; ++i)
    {
        ordered[i] = samples[i];
    }

    std::sort(ordered, ordered + sample_count);

    const std::size_t trim = sample_count / 10;
    const std::size_t begin = trim;
    const std::size_t end = sample_count - trim;

    double sum = 0.0;

    for (std::size_t i = begin; i < end; ++i)
    {
        sum += ordered[i];
    }

    range = ordered[end - 1] - ordered[begin];

    return static_cast<float>(
        sum / static_cast<double>(end - begin)
    );
}

bool collect_stable_weight(float& accepted_grams)
{
    const uint64_t current = now_ms();

    if (
        last_sample_ms != 0 &&
        current - last_sample_ms < SAMPLE_INTERVAL_MS
    )
    {
        return false;
    }

    last_sample_ms = current;

    float grams = 0.0f;

    if (!scale_read_weight_g(grams))
    {
        return false;
    }

    samples[sample_index] = grams;
    sample_index = (sample_index + 1) % SAMPLE_COUNT;

    if (sample_count < SAMPLE_COUNT)
    {
        ++sample_count;
    }

    if (sample_count < SAMPLE_COUNT)
    {
        return false;
    }

    float range_g = 0.0f;
    const float mean = trimmed_mean_and_range(range_g);

    const float drift_g = previous_mean_valid
        ? std::fabs(mean - previous_mean)
        : 0.0f;

    previous_mean = mean;
    previous_mean_valid = true;

    if (
        range_g > MAX_TRIMMED_RANGE_G ||
        drift_g > MAX_MEAN_DRIFT_G
    )
    {
        stable_since_ms = 0;
        return false;
    }

    if (stable_since_ms == 0)
    {
        stable_since_ms = current;
        return false;
    }

    if (current - stable_since_ms < REQUIRED_STABLE_MS)
    {
        return false;
    }

    accepted_grams = mean;
    return true;
}

void trigger_screen()
{
    screen_active = true;
    screen_started_ms = now_ms();

    display_show_consumption_screen(
        last_consumed_ml,
        daily_consumed_ml,
        daily_goal_ml
    );
}

void process_weight(float total_grams)
{
    const ScaleCalibration& calibration = scale_get_calibration();

    if (
        !calibration.valid ||
        calibration.capacity_ml <= 0.0f ||
        calibration.empty_bottle_g <= 0.0f
    )
    {
        ESP_LOGW(TAG, "Bottle not learned; consumption calculation skipped");
        return;
    }

    /* Arduino formula: water = total scale weight - empty bottle weight. */
    float water_ml = total_grams - calibration.empty_bottle_g;

    water_ml = std::clamp(
        water_ml,
        0.0f,
        calibration.capacity_ml + CAPACITY_NOISE_ALLOWANCE_ML
    );

    ESP_LOGI(
        TAG,
        "total=%.2fg empty=%.2fg water=%.2fml previous=%.2fml",
        total_grams,
        calibration.empty_bottle_g,
        water_ml,
        last_remaining_ml
    );

    /* Arduino first-reading behaviour: establish baseline only. */
    if (last_remaining_ml <= 0.0f)
    {
        last_remaining_ml = water_ml;
        ESP_LOGI(TAG, "First reading; baseline set to %.1fml", water_ml);
        return;
    }

    const float consumed_ml = last_remaining_ml - water_ml;

    if (
        consumed_ml >= MIN_CONSUMPTION_ML &&
        consumed_ml <= calibration.capacity_ml
    )
    {
        last_consumed_ml = static_cast<uint32_t>(std::lround(consumed_ml));
        daily_consumed_ml += last_consumed_ml;
        save_daily();

        ESP_LOGI(
            TAG,
            "Consumed %lu ml; daily total %lu/%lu ml",
            static_cast<unsigned long>(last_consumed_ml),
            static_cast<unsigned long>(daily_consumed_ml),
            static_cast<unsigned long>(daily_goal_ml)
        );

        trigger_screen();
    }
    else if (consumed_ml < REFILL_THRESHOLD_ML)
    {
        ESP_LOGI(
            TAG,
            "Refill detected: %.1fml -> %.1fml",
            last_remaining_ml,
            water_ml
        );
    }
    else
    {
        ESP_LOGI(
            TAG,
            "Change %.1fml ignored by Arduino thresholds",
            consumed_ml
        );
    }

    /* Arduino always updates the baseline after classification. */
    last_remaining_ml = water_ml;
}
}

esp_err_t consumption_tracker_init()
{
    if (!scale_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    load_daily();

    const ScaleCalibration& calibration = scale_get_calibration();
    if (calibration.valid && calibration.capacity_ml > 0.0f)
    {
        last_remaining_ml = calibration.capacity_ml;
    }

    initialized = true;

    ESP_LOGI(
        TAG,
        "Initialized; daily=%lu goal=%lu settle=%lums screen=%lums",
        static_cast<unsigned long>(daily_consumed_ml),
        static_cast<unsigned long>(daily_goal_ml),
        static_cast<unsigned long>(BOTTLE_SETTLING_DELAY_MS),
        static_cast<unsigned long>(CONSUMPTION_SCREEN_DURATION_MS)
    );

    return ESP_OK;
}

void consumption_tracker_set_enabled(bool enabled)
{
    if (tracking_enabled == enabled)
    {
        return;
    }

    tracking_enabled = enabled;
    state = TrackerState::IDLE;
    reset_measurement_window();

    /* Re-synchronise with the current filtered IR state after calibration. */
    dock_state_known = false;

    if (!tracking_enabled)
    {
        screen_active = false;
    }
}

void consumption_tracker_set_initial_remaining(float remaining_ml)
{
    if (remaining_ml < 0.0f)
    {
        remaining_ml = 0.0f;
    }

    last_remaining_ml = remaining_ml;
    ESP_LOGI(TAG, "Consumption baseline set to %.1f ml", last_remaining_ml);
}

void consumption_tracker_set_docked(bool new_docked)
{
    if (!initialized || !tracking_enabled)
    {
        return;
    }

    if (!dock_state_known)
    {
        dock_state_known = true;
        docked = new_docked;
        return;
    }

    if (new_docked == docked)
    {
        return;
    }

    const bool was_docked = docked;
    docked = new_docked;

    if (!docked)
    {
        /* Bottle removed: cancel a pending return measurement. */
        state = TrackerState::IDLE;
        reset_measurement_window();
        ESP_LOGI(TAG, "Bottle removed");
        return;
    }

    if (!was_docked && docked)
    {
        /* Removed -> docked edge: mandatory five-second settle delay. */
        state = TrackerState::SETTLING;
        settling_started_ms = now_ms();
        reset_measurement_window();
        ESP_LOGI(TAG, "Bottle returned; settling for 5 seconds");
    }
}

void consumption_tracker_update()
{
    if (!initialized || !tracking_enabled)
    {
        return;
    }

    reset_for_new_day_if_needed();

    const uint64_t current = now_ms();

    if (
        screen_active &&
        current - screen_started_ms >= CONSUMPTION_SCREEN_DURATION_MS
    )
    {
        screen_active = false;
    }

    if (state == TrackerState::IDLE)
    {
        return;
    }

    if (!docked)
    {
        state = TrackerState::IDLE;
        reset_measurement_window();
        return;
    }

    if (state == TrackerState::SETTLING)
    {
        if (current - settling_started_ms < BOTTLE_SETTLING_DELAY_MS)
        {
            return;
        }

        state = TrackerState::MEASURING;
        measuring_started_ms = current;
        reset_measurement_window();
        ESP_LOGI(TAG, "Settling complete; reading stable HX711 weight");
    }

    if (state != TrackerState::MEASURING)
    {
        return;
    }

    if (current - measuring_started_ms >= MEASUREMENT_TIMEOUT_MS)
    {
        ESP_LOGW(TAG, "Stable weight timeout; measurement cancelled");
        state = TrackerState::IDLE;
        reset_measurement_window();
        return;
    }

    float total_grams = 0.0f;

    if (!collect_stable_weight(total_grams))
    {
        return;
    }

    state = TrackerState::IDLE;
    reset_measurement_window();
    process_weight(total_grams);
}

bool consumption_tracker_screen_active()
{
    return screen_active;
}

void consumption_tracker_render_screen()
{
    if (!screen_active)
    {
        return;
    }

    display_show_consumption_screen(
        last_consumed_ml,
        daily_consumed_ml,
        daily_goal_ml
    );
}

uint32_t consumption_tracker_last_consumed_ml()
{
    return last_consumed_ml;
}

uint32_t consumption_tracker_daily_consumed_ml()
{
    return daily_consumed_ml;
}

uint32_t consumption_tracker_daily_goal_ml()
{
    return daily_goal_ml;
}

void consumption_tracker_set_daily_goal_ml(uint32_t goal_ml)
{
    if (goal_ml == 0)
    {
        return;
    }

    daily_goal_ml = goal_ml;
    save_daily();
}

void consumption_tracker_reset_daily()
{
    daily_consumed_ml = 0;
    last_consumed_ml = 0;
    save_daily();
}