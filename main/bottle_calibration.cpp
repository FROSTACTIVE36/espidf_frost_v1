#include "bottle_calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "display.hpp"
#include "audio_manager.hpp"
#include "consumption_tracker.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "scale.hpp"

namespace
{
const char* TAG = "BOTTLE_CAL";

constexpr const char* NVS_NAMESPACE = "bottle_cal";
constexpr const char* NVS_KEY = "values";

constexpr uint32_t SAMPLE_INTERVAL_MS = 80;
constexpr std::size_t WINDOW_SIZE = 20;
constexpr uint32_t REQUIRED_STABLE_MS = 1400;
constexpr uint32_t STEP_TIMEOUT_MS = 30000;

/*
 * ADC values fluctuate continuously. We accept a window when:
 * - the middle 80% span is within this tolerance
 * - consecutive trimmed means drift slowly
 */
constexpr float MAX_TRIMMED_RANGE_G = 8.0f;
constexpr float MAX_MEAN_DRIFT_G = 2.5f;
constexpr float MIN_EMPTY_BOTTLE_G = 5.0f;
constexpr float MIN_CAPACITY_G = 50.0f;

struct StoredCalibration
{
    uint32_t version;
    int32_t offset_raw;
    float grams_per_raw_unit;
    float empty_bottle_g;
    float full_bottle_g;
    float capacity_ml;
};

BottleCalibrationState state = BottleCalibrationState::IDLE;
bool docked = false;

uint64_t state_started_ms = 0;
uint64_t last_sample_ms = 0;
uint64_t stable_since_ms = 0;

int32_t raw_samples[WINDOW_SIZE] = {};
float gram_samples[WINDOW_SIZE] = {};
std::size_t sample_count = 0;
std::size_t sample_index = 0;

float previous_mean = 0.0f;
bool previous_mean_valid = false;

ScaleCalibration working_calibration = {};

uint64_t now_ms()
{
    return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
}

void reset_window()
{
    sample_count = 0;
    sample_index = 0;
    stable_since_ms = 0;
    previous_mean = 0.0f;
    previous_mean_valid = false;
    std::memset(raw_samples, 0, sizeof(raw_samples));
    std::memset(gram_samples, 0, sizeof(gram_samples));
}

void enter_state(BottleCalibrationState next)
{
    state = next;
    state_started_ms = now_ms();
    last_sample_ms = 0;
    reset_window();

    switch (state)
    {
        case BottleCalibrationState::WAIT_REMOVE_FOR_TARE:
            display_show_calibration_remove_bottle();
            audio_manager_play_calibration_announcement(
                CalibrationAnnouncement::REMOVE_BOTTLE
            );
            break;

        case BottleCalibrationState::TARING:
            display_show_calibration_measuring("TARING", "Keep dock empty");
            break;

        case BottleCalibrationState::WAIT_EMPTY_BOTTLE:
            display_show_calibration_place_empty();
            audio_manager_play_calibration_announcement(
                CalibrationAnnouncement::PLACE_EMPTY_BOTTLE
            );
            break;

        case BottleCalibrationState::MEASURE_EMPTY:
            display_show_calibration_measuring(
                "EMPTY BOTTLE",
                "Keep bottle still"
            );
            break;

        case BottleCalibrationState::WAIT_REMOVE_TO_FILL:
            display_show_calibration_fill_bottle();
            break;

        case BottleCalibrationState::WAIT_FULL_BOTTLE:
            display_show_calibration_place_full();
            audio_manager_play_calibration_announcement(
                CalibrationAnnouncement::PLACE_FULL_BOTTLE
            );
            break;

        case BottleCalibrationState::MEASURE_FULL:
            display_show_calibration_measuring(
                "FULL BOTTLE",
                "Keep bottle still"
            );
            break;

        case BottleCalibrationState::COMPLETE:
            display_show_calibration_complete(
                working_calibration.capacity_ml
            );
            audio_manager_play_calibration_announcement(
                CalibrationAnnouncement::CALIBRATION_SUCCESS
            );
            break;

        case BottleCalibrationState::ERROR:
            display_show_calibration_error("Calibration failed");
            audio_manager_play_calibration_announcement(
                CalibrationAnnouncement::CALIBRATION_FAILED
            );
            break;

        case BottleCalibrationState::CANCELLED:
            display_show_calibration_error("Calibration cancelled");
            break;

        case BottleCalibrationState::IDLE:
        default:
            break;
    }

    ESP_LOGI(TAG, "Calibration state=%u", static_cast<unsigned>(state));
}

bool save_calibration(const ScaleCalibration& calibration)
{
    StoredCalibration stored = {};
    stored.version = 1;
    stored.offset_raw = calibration.offset_raw;
    stored.grams_per_raw_unit = calibration.grams_per_raw_unit;
    stored.empty_bottle_g = calibration.empty_bottle_g;
    stored.full_bottle_g = calibration.full_bottle_g;
    stored.capacity_ml = calibration.capacity_ml;

    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(
        NVS_NAMESPACE,
        NVS_READWRITE,
        &handle
    );

    if (error != ESP_OK)
    {
        return false;
    }

    error = nvs_set_blob(
        handle,
        NVS_KEY,
        &stored,
        sizeof(stored)
    );

    if (error == ESP_OK)
    {
        error = nvs_commit(handle);
    }

    nvs_close(handle);
    return error == ESP_OK;
}

bool load_calibration()
{
    nvs_handle_t handle = 0;
    esp_err_t error = nvs_open(
        NVS_NAMESPACE,
        NVS_READONLY,
        &handle
    );

    if (error != ESP_OK)
    {
        return false;
    }

    StoredCalibration stored = {};
    std::size_t size = sizeof(stored);

    error = nvs_get_blob(
        handle,
        NVS_KEY,
        &stored,
        &size
    );

    nvs_close(handle);

    if (
        error != ESP_OK ||
        size != sizeof(stored) ||
        stored.version != 1
    )
    {
        return false;
    }

    ScaleCalibration loaded;
    loaded.valid = true;
    loaded.offset_raw = stored.offset_raw;
    loaded.grams_per_raw_unit = stored.grams_per_raw_unit;
    loaded.empty_bottle_g = stored.empty_bottle_g;
    loaded.full_bottle_g = stored.full_bottle_g;
    loaded.capacity_ml = stored.capacity_ml;

    scale_set_calibration(loaded);
    working_calibration = loaded;
    consumption_tracker_set_initial_remaining(loaded.capacity_ml);

    ESP_LOGI(
        TAG,
        "Loaded calibration: empty=%.1fg full=%.1fg capacity=%.0fml",
        loaded.empty_bottle_g,
        loaded.full_bottle_g,
        loaded.capacity_ml
    );

    return true;
}

void add_sample(int32_t raw, float grams)
{
    raw_samples[sample_index] = raw;
    gram_samples[sample_index] = grams;

    sample_index = (sample_index + 1) % WINDOW_SIZE;

    if (sample_count < WINDOW_SIZE)
    {
        ++sample_count;
    }
}

template <typename T>
float trimmed_mean_and_range(
    const T* source,
    std::size_t count,
    float& trimmed_range
)
{
    T values[WINDOW_SIZE] = {};

    for (std::size_t i = 0; i < count; ++i)
    {
        values[i] = source[i];
    }

    std::sort(values, values + count);

    const std::size_t trim = count / 10;
    const std::size_t begin = trim;
    const std::size_t end = count - trim;

    double sum = 0.0;

    for (std::size_t i = begin; i < end; ++i)
    {
        sum += static_cast<double>(values[i]);
    }

    trimmed_range =
        static_cast<float>(values[end - 1]) -
        static_cast<float>(values[begin]);

    return static_cast<float>(
        sum / static_cast<double>(end - begin)
    );
}

bool collect_stable_raw(int32_t& accepted_raw)
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

    int32_t raw = 0;

    if (!scale_read_raw(raw))
    {
        return false;
    }

    add_sample(raw, 0.0f);

    if (sample_count < WINDOW_SIZE)
    {
        return false;
    }

    float raw_range = 0.0f;
    const float mean_raw =
        trimmed_mean_and_range(raw_samples, sample_count, raw_range);

    const float range_g =
        raw_range * DEFAULT_SCALE_CALIB_G_PER_UNIT;

    const float drift_g =
        previous_mean_valid
            ? std::fabs(
                (mean_raw - previous_mean) *
                DEFAULT_SCALE_CALIB_G_PER_UNIT
              )
            : 0.0f;

    previous_mean = mean_raw;
    previous_mean_valid = true;

    const bool acceptable =
        range_g <= MAX_TRIMMED_RANGE_G &&
        drift_g <= MAX_MEAN_DRIFT_G;

    if (!acceptable)
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

    accepted_raw = static_cast<int32_t>(std::lround(mean_raw));
    return true;
}

bool collect_stable_grams(float& accepted_grams)
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

    add_sample(0, grams);

    if (sample_count < WINDOW_SIZE)
    {
        return false;
    }

    float range_g = 0.0f;
    const float mean =
        trimmed_mean_and_range(gram_samples, sample_count, range_g);

    const float drift_g =
        previous_mean_valid
            ? std::fabs(mean - previous_mean)
            : 0.0f;

    previous_mean = mean;
    previous_mean_valid = true;

    const bool acceptable =
        range_g <= MAX_TRIMMED_RANGE_G &&
        drift_g <= MAX_MEAN_DRIFT_G;

    if (!acceptable)
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

bool timed_out()
{
    return now_ms() - state_started_ms >= STEP_TIMEOUT_MS;
}
}

esp_err_t bottle_calibration_init()
{
    working_calibration = {};
    working_calibration.grams_per_raw_unit =
        DEFAULT_SCALE_CALIB_G_PER_UNIT;

    load_calibration();
    state = BottleCalibrationState::IDLE;
    return ESP_OK;
}

bool bottle_calibration_start()
{
    if (!scale_is_initialized())
    {
        return false;
    }

    working_calibration = {};
    working_calibration.grams_per_raw_unit =
        DEFAULT_SCALE_CALIB_G_PER_UNIT;

    enter_state(BottleCalibrationState::WAIT_REMOVE_FOR_TARE);
    return true;
}

void bottle_calibration_cancel()
{
    if (!bottle_calibration_is_active())
    {
        return;
    }

    enter_state(BottleCalibrationState::CANCELLED);
}

void bottle_calibration_set_docked(bool value)
{
    docked = value;
}

void bottle_calibration_update()
{
    switch (state)
    {
        case BottleCalibrationState::WAIT_REMOVE_FOR_TARE:
            if (!docked)
            {
                enter_state(BottleCalibrationState::TARING);
            }
            break;

        case BottleCalibrationState::TARING:
        {
            int32_t tare_raw = 0;

            if (collect_stable_raw(tare_raw))
            {
                working_calibration.offset_raw = tare_raw;
                working_calibration.valid = false;
                scale_set_calibration(working_calibration);

                ESP_LOGI(TAG, "Tare raw=%ld", static_cast<long>(tare_raw));
                enter_state(BottleCalibrationState::WAIT_EMPTY_BOTTLE);
            }
            else if (timed_out())
            {
                enter_state(BottleCalibrationState::ERROR);
            }
            break;
        }

        case BottleCalibrationState::WAIT_EMPTY_BOTTLE:
            if (docked)
            {
                enter_state(BottleCalibrationState::MEASURE_EMPTY);
            }
            break;

        case BottleCalibrationState::MEASURE_EMPTY:
        {
            if (!docked)
            {
                enter_state(BottleCalibrationState::WAIT_EMPTY_BOTTLE);
                break;
            }

            float empty_g = 0.0f;

            if (collect_stable_grams(empty_g))
            {
                if (empty_g < MIN_EMPTY_BOTTLE_G)
                {
                    enter_state(BottleCalibrationState::ERROR);
                    break;
                }

                working_calibration.empty_bottle_g = empty_g;

                ESP_LOGI(TAG, "Empty bottle=%.1fg", empty_g);
                enter_state(BottleCalibrationState::WAIT_REMOVE_TO_FILL);
            }
            else if (timed_out())
            {
                enter_state(BottleCalibrationState::ERROR);
            }
            break;
        }

        case BottleCalibrationState::WAIT_REMOVE_TO_FILL:
            if (!docked)
            {
                enter_state(BottleCalibrationState::WAIT_FULL_BOTTLE);
            }
            break;

        case BottleCalibrationState::WAIT_FULL_BOTTLE:
            if (docked)
            {
                enter_state(BottleCalibrationState::MEASURE_FULL);
            }
            break;

        case BottleCalibrationState::MEASURE_FULL:
        {
            if (!docked)
            {
                enter_state(BottleCalibrationState::WAIT_FULL_BOTTLE);
                break;
            }

            float full_g = 0.0f;

            if (collect_stable_grams(full_g))
            {
                const float capacity =
                    full_g - working_calibration.empty_bottle_g;

                if (capacity < MIN_CAPACITY_G)
                {
                    enter_state(BottleCalibrationState::ERROR);
                    break;
                }

                working_calibration.full_bottle_g = full_g;
                working_calibration.capacity_ml = capacity;
                working_calibration.valid = true;

                scale_set_calibration(working_calibration);
                consumption_tracker_set_initial_remaining(capacity);

                if (!save_calibration(working_calibration))
                {
                    enter_state(BottleCalibrationState::ERROR);
                    break;
                }

                ESP_LOGI(
                    TAG,
                    "Full=%.1fg capacity=%.0fml",
                    full_g,
                    capacity
                );

                enter_state(BottleCalibrationState::COMPLETE);
            }
            else if (timed_out())
            {
                enter_state(BottleCalibrationState::ERROR);
            }
            break;
        }

        case BottleCalibrationState::COMPLETE:
        case BottleCalibrationState::CANCELLED:
        case BottleCalibrationState::ERROR:
            /*
             * Keep the final full-screen message for 3 seconds.
             */
            if (now_ms() - state_started_ms >= 3000)
            {
                state = BottleCalibrationState::IDLE;
            }
            break;

        case BottleCalibrationState::IDLE:
        default:
            break;
    }
}

bool bottle_calibration_is_active()
{
    return state != BottleCalibrationState::IDLE;
}

BottleCalibrationState bottle_calibration_get_state()
{
    return state;
}

void bottle_calibration_get_status(
    char* destination,
    std::size_t destination_size
)
{
    if (destination == nullptr || destination_size == 0)
    {
        return;
    }

    const ScaleCalibration& calibration =
        scale_get_calibration();

    std::snprintf(
        destination,
        destination_size,
        "BOTTLE:state=%u,valid=%u,empty=%.1f,full=%.1f,capacity=%.0f",
        static_cast<unsigned>(state),
        calibration.valid ? 1U : 0U,
        calibration.empty_bottle_g,
        calibration.full_bottle_g,
        calibration.capacity_ml
    );
}