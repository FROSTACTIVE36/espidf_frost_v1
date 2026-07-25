#include "display.hpp"
#include "reminder_types.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "LovyanGFX.hpp"

#include "images/frost_logo.h"
#include "images/clock_bg.h"
#include "images/font.h"
#include "images/font_regular.h"
#include "images/font_small.h"
#include "action_log.hpp"
#include "images/drinkwater1.h"
#include "images/image_time_to_stretch_inverted.h"
#include "images/rule.h"
#include "images/short_walk.h"
#include "images/medication_background.h"
#include "images/custom_background.h"
#include "images/meditation.h"
#include "images/pomodoro_focus_bg.h"
#include "images/pomodoro_break_bg.h"
#include "images/hydration_consumption.h"
#include "images/bottle_clean.h"


static const char *TAG = "FROST_DISPLAY";

/* =========================================================
 * Display configuration
 * ========================================================= */

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240

#define DISPLAY_SCLK_PIN GPIO_NUM_12
#define DISPLAY_MOSI_PIN GPIO_NUM_11
#define DISPLAY_MISO_PIN GPIO_NUM_NC
#define DISPLAY_DC_PIN   GPIO_NUM_13
#define DISPLAY_CS_PIN   GPIO_NUM_10
#define DISPLAY_RST_PIN  GPIO_NUM_4

/* =========================================================
 * Clock layout
 * ========================================================= */

static constexpr int CLOCK_CENTER_X = 120;
static constexpr int CLOCK_CENTER_Y = 120;

static constexpr int CLOCK_PROGRESS_RADIUS = 112;
static constexpr int CLOCK_PROGRESS_WIDTH = 6;

/*
 * Pomodoro progress geometry copied from the working Arduino version.
 */
static constexpr int POMODORO_CENTER_X = 118;
static constexpr int POMODORO_CENTER_Y = 120;
static constexpr int POMODORO_CLOCK_RADIUS = 105;
static constexpr int POMODORO_PROGRESS_RADIUS = POMODORO_CLOCK_RADIUS + 2;
static constexpr int POMODORO_PROGRESS_WIDTH = 7;

static constexpr uint16_t POMODORO_FOCUS_ARC_COLOR = 0x8260;
static constexpr uint16_t POMODORO_BREAK_ARC_COLOR = TFT_GREEN;

/* =========================================================
 * LovyanGFX device
 * ========================================================= */

class LGFX : public lgfx::LGFX_Device
{
private:
    lgfx::Panel_GC9A01 panel_;
    lgfx::Bus_SPI bus_;

public:
    LGFX()
    {
        configure_bus();
        configure_panel();
        setPanel(&panel_);
    }

private:
    void configure_bus()
    {
        auto config = bus_.config();

        config.spi_host = SPI2_HOST;
        config.spi_mode = 0;

        config.freq_write = 40000000;
        config.freq_read = 16000000;

        config.spi_3wire = false;
        config.use_lock = true;
        config.dma_channel = SPI_DMA_CH_AUTO;

        config.pin_sclk = DISPLAY_SCLK_PIN;
        config.pin_mosi = DISPLAY_MOSI_PIN;
        config.pin_miso = DISPLAY_MISO_PIN;
        config.pin_dc = DISPLAY_DC_PIN;

        bus_.config(config);
        panel_.setBus(&bus_);
    }

    void configure_panel()
    {
        auto config = panel_.config();

        config.pin_cs = DISPLAY_CS_PIN;
        config.pin_rst = DISPLAY_RST_PIN;
        config.pin_busy = -1;

        config.memory_width = DISPLAY_WIDTH;
        config.memory_height = DISPLAY_HEIGHT;

        config.panel_width = DISPLAY_WIDTH;
        config.panel_height = DISPLAY_HEIGHT;

        config.offset_x = 0;
        config.offset_y = 0;

        /*
         * Keep these values because they are already working
         * with your current display.
         */
        config.rgb_order = false;
        config.invert = false;

        config.readable = false;
        config.bus_shared = false;

        panel_.config(config);
    }
};

/* =========================================================
 * Display objects
 * ========================================================= */

static LGFX display;
static LGFX_Sprite screen(&display);

static bool display_ready = false;
static bool sprite_ready = false;

/* =========================================================
 * Initialize display
 * ========================================================= */

bool display_init()
{
    ESP_LOGI(TAG, "Initializing GC9A01");

    if (!display.init())
    {
        ESP_LOGE(TAG, "Display initialization failed");
        return false;
    }

    display.setRotation(2);
    display.setSwapBytes(true);
    display.fillScreen(TFT_BLACK);

    display_ready = true;

    /*
     * Create a full-screen RGB565 sprite in external PSRAM.
     * Required bytes: 240 x 240 x 2 = 115200 bytes.
     */
    constexpr size_t SPRITE_BYTES =
        static_cast<size_t>(DISPLAY_WIDTH) *
        static_cast<size_t>(DISPLAY_HEIGHT) *
        sizeof(uint16_t);

    const size_t psram_total =
        heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    const size_t psram_free_before =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    const size_t psram_largest_before =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    const size_t internal_free_before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    ESP_LOGI(
        TAG,
        "Memory before sprite: PSRAM total=%u free=%u largest=%u, internal free=%u",
        static_cast<unsigned>(psram_total),
        static_cast<unsigned>(psram_free_before),
        static_cast<unsigned>(psram_largest_before),
        static_cast<unsigned>(internal_free_before)
    );

    if (
        psram_total == 0 ||
        psram_largest_before < SPRITE_BYTES
    )
    {
        ESP_LOGE(
            TAG,
            "PSRAM unavailable or too small for sprite: need=%u bytes",
            static_cast<unsigned>(SPRITE_BYTES)
        );

        ESP_LOGE(
            TAG,
            "Enable ESP PSRAM and make it available to heap_caps/malloc in menuconfig"
        );

        sprite_ready = false;
        return true;
    }

    screen.setColorDepth(16);

    /* Must be called before createSprite(). */
    screen.setPsram(true);
    screen.setSwapBytes(true);

    void* sprite_buffer =
        screen.createSprite(
            DISPLAY_WIDTH,
            DISPLAY_HEIGHT
        );

    if (sprite_buffer == nullptr)
    {
        ESP_LOGE(
            TAG,
            "PSRAM sprite creation failed: need=%u, free=%u, largest=%u",
            static_cast<unsigned>(SPRITE_BYTES),
            static_cast<unsigned>(
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
            ),
            static_cast<unsigned>(
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)
            )
        );

        sprite_ready = false;
        return true;
    }

    sprite_ready = true;

    ESP_LOGI(
        TAG,
        "Display sprite created with PSRAM enabled: buffer=%p, PSRAM free after=%u",
        sprite_buffer,
        static_cast<unsigned>(
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
        )
    );

    ESP_LOGI(TAG, "Display and sprite initialized");

    return true;
}

/* =========================================================
 * Clear display
 * ========================================================= */

void display_clear(uint16_t color)
{
    if (!display_ready)
    {
        return;
    }

    display.fillScreen(color);
}

/* =========================================================
 * Boot logo
 * ========================================================= */

void display_show_frost_logo()
{
    if (!display_ready)
    {
        return;
    }

    ESP_LOGI(TAG, "Showing Frost boot logo");

    display.setSwapBytes(true);

    display.pushImage(
        0,
        0,
        FROST_LOGO_WIDTH,
        FROST_LOGO_HEIGHT,
        frost_logo_data
    );
}

void display_show_hydration_reminder()
{
    display.startWrite();

    display.pushImage(
        0,
        0,
        240,
        240,
        drinkwater1_data
    );

    display.endWrite();
}

void display_show_stretch_reminder()
{
    display.startWrite();

    display.pushImage(
        0,
        0,
        240,
        240,
        image_time_to_stretch_data
    );

    display.endWrite();
}

void display_show_eye_reminder()
{
    display.startWrite();

    display.pushImage(
        0,
        0,
        240,
        240,
        rule_data
    );

    display.endWrite();
}

void display_show_walk_reminder()
{
    if (!display_ready)
    {
        return;
    }

    display.startWrite();

    display.pushImage(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        short_walk_data
    );

    display.endWrite();
}

void display_show_bottle_clean_reminder()
{
    if (!display_ready)
    {
        return;
    }

    display.startWrite();

    display.pushImage(
        0,
        0,
        240,
        240,
        bottle_clean_data
    );

    display.endWrite();
}


/* =========================================================
 * Reminder text helpers
 * ========================================================= */

/*
 * text_align values:
 *
 * 0 = left
 * 1 = center
 * 2 = right
 */
static lgfx::textdatum_t get_reminder_text_datum(
    uint8_t text_align
)
{
    switch (text_align)
    {
        case 0:
            return lgfx::textdatum_t::middle_left;

        case 2:
            return lgfx::textdatum_t::middle_right;

        case 1:
        default:
            return lgfx::textdatum_t::middle_center;
    }
}

/*
 * Draws one line of reminder text.
 *
 * text_x and text_y come from the JSON.
 */
static void draw_single_line_reminder_text(
    const char* label,
    int16_t text_x,
    int16_t text_y,
    uint8_t text_size,
    uint16_t text_color,
    uint8_t text_align
)
{
    if (label == nullptr)
    {
        label = "";
    }

    screen.setTextDatum(
        get_reminder_text_datum(
            text_align
        )
    );

    screen.setTextColor(
        text_color
    );

    screen.setTextSize(
        text_size > 0
            ? text_size
            : 1
    );

    screen.drawString(
        label,
        text_x,
        text_y
    );
}

/*
 * Wraps long labels into multiple lines.
 *
 * text_x is used as the center X position.
 * text_y is the first line Y position.
 */
static void draw_wrapped_reminder_text(
    const char* label,
    int16_t text_x,
    int16_t text_y,
    uint8_t text_size,
    uint16_t text_color,
    uint16_t maximum_width
)
{
    if (
        label == nullptr ||
        label[0] == '\0'
    )
    {
        return;
    }

    const uint8_t final_text_size =
        text_size > 0
            ? text_size
            : 1;

    screen.setTextSize(
        final_text_size
    );

    screen.setTextColor(
        text_color
    );

    screen.setTextDatum(
        lgfx::textdatum_t::middle_center
    );

    char source[REMINDER_LABEL_LENGTH] = {};
    char current_line[REMINDER_LABEL_LENGTH] = {};
    char test_line[REMINDER_LABEL_LENGTH] = {};

    snprintf(
        source,
        sizeof(source),
        "%s",
        label
    );

    int16_t line_height =
        static_cast<int16_t>(screen.fontHeight());

    if (line_height <= 0)
    {
        line_height = static_cast<int16_t>(12 * final_text_size);
    }

    int16_t current_y = text_y;

    char* save_pointer = nullptr;

    char* word =
        strtok_r(
            source,
            " ",
            &save_pointer
        );

    while (word != nullptr)
    {
        if (current_line[0] == '\0')
        {
            snprintf(
                test_line,
                sizeof(test_line),
                "%s",
                word
            );
        }
        else
        {
            strncpy(
                test_line,
                current_line,
                sizeof(test_line) - 1
            );

            test_line[sizeof(test_line) - 1] = '\0';

            size_t remaining =
                sizeof(test_line) -
                strlen(test_line) - 1;

            if (remaining > 0)
            {
                strncat(
                    test_line,
                    " ",
                    remaining
                );
            }

            remaining =
                sizeof(test_line) -
                strlen(test_line) - 1;

            if (remaining > 0)
            {
                strncat(
                    test_line,
                    word,
                    remaining
                );
            }
        }

        const int32_t measured_width =
            screen.textWidth(
                test_line
            );

        if (
            measured_width <= maximum_width ||
            current_line[0] == '\0'
        )
        {
            snprintf(
                current_line,
                sizeof(current_line),
                "%s",
                test_line
            );
        }
        else
        {
            screen.drawString(
                current_line,
                text_x,
                current_y
            );

            current_y += line_height;

            snprintf(
                current_line,
                sizeof(current_line),
                "%s",
                word
            );
        }

        word =
            strtok_r(
                nullptr,
                " ",
                &save_pointer
            );
    }

    if (current_line[0] != '\0')
    {
        screen.drawString(
            current_line,
            text_x,
            current_y
        );
    }
}

/*
 * Selects single-line or wrapped drawing.
 */
static void draw_configured_reminder_text(
    const char* label,
    int16_t text_x,
    int16_t text_y,
    uint8_t text_size,
    uint16_t text_color,
    uint8_t text_align,
    uint16_t text_width
)
{
    if (
        label == nullptr ||
        label[0] == '\0'
    )
    {
        return;
    }

    screen.setTextSize(
        text_size > 0
            ? text_size
            : 1
    );

    const int32_t measured_width =
        screen.textWidth(
            label
        );

    if (
        text_width > 0 &&
        measured_width >
            static_cast<int32_t>(
                text_width
            )
    )
    {
        draw_wrapped_reminder_text(
            label,
            text_x,
            text_y,
            text_size,
            text_color,
            text_width
        );

        return;
    }

    draw_single_line_reminder_text(
        label,
        text_x,
        text_y,
        text_size,
        text_color,
        text_align
    );
}

/* =========================================================
 * Medication reminder
 * ========================================================= */

void display_show_medication_reminder(
    const char* label,
    int16_t text_x,
    int16_t text_y,
    uint8_t text_size,
    uint16_t text_color,
    uint8_t text_align,
    uint16_t text_width
)
{
    if (!display_ready || !sprite_ready)
    {
        return;
    }

    ESP_LOGI(
        TAG,
        "Showing medication reminder: %s at x=%d y=%d",
        label != nullptr
            ? label
            : "",
        text_x,
        text_y
    );

    /*
     * Draw the medication background into the sprite.
     */
    screen.setSwapBytes(true);

    screen.pushImage(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        medication_background_data
    );

    /*
     * Match the custom-reminder and Arduino UI by using
     * font_regular for medication labels as well.
     */
    screen.loadFont(font_regular);
    screen.setTextSize(1);

    draw_configured_reminder_text(
        label,
        text_x,
        text_y,
        1,
        text_color,
        text_align,
        text_width
    );

    screen.unloadFont();

    /*
     * Send completed sprite to the display.
     */
    display.startWrite();

    screen.pushSprite(
        0,
        0
    );

    display.endWrite();
}

/* =========================================================
 * Custom reminder
 * ========================================================= */

void display_show_custom_reminder(
    const char* label,
    int16_t text_x,
    int16_t text_y,
    uint8_t text_size,
    uint16_t text_color,
    uint8_t text_align,
    uint16_t text_width
)
{
    if (!display_ready || !sprite_ready)
    {
        return;
    }

    ESP_LOGI(
        TAG,
        "Showing custom reminder: %s at x=%d y=%d",
        label != nullptr
            ? label
            : "",
        text_x,
        text_y
    );

    /*
     * Draw the custom reminder background into the sprite.
     */
    screen.setSwapBytes(true);

    screen.pushImage(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        custom_background
    );

    /*
     * Match the Arduino custom-reminder UI by using font_regular.
     * The wrapped-text helper now uses the font's actual height.
     */
    screen.loadFont(font_regular);
    screen.setTextSize(1);

    draw_configured_reminder_text(
        label,
        text_x,
        text_y,
        1,
        text_color,
        text_align,
        text_width
    );

    screen.unloadFont();

    /*
     * Push completed sprite.
     */
    display.startWrite();

    screen.pushSprite(
        0,
        0
    );

    display.endWrite();
}

/* =========================================================
 * Meditation reminder
 * ========================================================= */

void display_show_meditation_reminder()
{
    if (!display_ready)
    {
        return;
    }

    ESP_LOGI(
        TAG,
        "Showing meditation reminder"
    );

    /*
     * Meditation is already a complete image,
     * so it can be sent directly to the display.
     */
    display.startWrite();

    display.pushImage(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        meditation_data
    );

    display.endWrite();
}

/* =========================================================
 * Pomodoro display
 * ========================================================= */

static void draw_pomodoro_counter(
    const uint16_t* background,
    uint32_t remaining_seconds,
    uint32_t total_seconds,
    const PomodoroCounterStyle& style,
    uint16_t arc_color
)
{
    if (
        !display_ready ||
        !sprite_ready
    )
    {
        return;
    }

    const uint32_t minutes =
        remaining_seconds / 60U;

    const uint32_t seconds =
        remaining_seconds % 60U;

    char timer_text[16] = {};

    std::snprintf(
        timer_text,
        sizeof(timer_text),
        "%02lu:%02lu",
        static_cast<unsigned long>(minutes),
        static_cast<unsigned long>(seconds)
    );

    /*
     * Redraw the complete background before drawing the new
     * timer and progress arc.
     */
    screen.setSwapBytes(true);

    screen.pushImage(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        background
    );

    /*
     * Match the circular progress indicator from the Arduino
     * Pomodoro screen.
     *
     * It begins at 12 o'clock (270 degrees) and grows clockwise
     * as the current focus or break stage elapses.
     */
    uint32_t elapsed_seconds = 0;

    if (total_seconds > remaining_seconds)
    {
        elapsed_seconds =
            total_seconds - remaining_seconds;
    }

    float progress = 0.0f;

    if (total_seconds > 0U)
    {
        progress =
            static_cast<float>(elapsed_seconds) /
            static_cast<float>(total_seconds);
    }

    if (progress < 0.0f)
    {
        progress = 0.0f;
    }
    else if (progress > 1.0f)
    {
        progress = 1.0f;
    }

    const float start_angle = 270.0f;
    const float end_angle =
        start_angle + progress * 360.0f;

    /*
     * Keep the thin circular frame used by the Arduino UI.
     */
    screen.drawCircle(
        POMODORO_CENTER_X,
        POMODORO_CENTER_Y,
        POMODORO_CLOCK_RADIUS,
        TFT_BLACK
    );

    /*
     * Avoid calling fillArc with a zero-length angle because
     * some LovyanGFX versions may interpret equal angles as a
     * complete circle.
     */
    if (elapsed_seconds > 0U)
    {
        screen.fillArc(
            POMODORO_CENTER_X,
            POMODORO_CENTER_Y,
            POMODORO_PROGRESS_RADIUS,
            POMODORO_PROGRESS_RADIUS +
                POMODORO_PROGRESS_WIDTH - 1,
            start_angle,
            end_angle,
            arc_color
        );
    }

    /*
     * Use the same custom font as the home clock.
     */
    screen.loadFont(font);

    screen.setTextDatum(
        get_reminder_text_datum(
            style.text_align
        )
    );

    screen.setTextColor(
        style.text_color
    );

    screen.setTextSize(1);

    screen.drawString(
        timer_text,
        style.x,
        style.y
    );

    screen.unloadFont();

    display.startWrite();

    screen.pushSprite(
        0,
        0
    );

    display.endWrite();
}

void display_show_pomodoro_focus(
    uint32_t remaining_seconds,
    uint32_t total_seconds,
    const PomodoroCounterStyle& style
)
{
    draw_pomodoro_counter(
        pomodoro_focus_bg_data,
        remaining_seconds,
        total_seconds,
        style,
        POMODORO_FOCUS_ARC_COLOR
    );
}

void display_show_pomodoro_break(
    uint32_t remaining_seconds,
    uint32_t total_seconds,
    const PomodoroCounterStyle& style
)
{
    draw_pomodoro_counter(
        pomodoro_break_bg_data,
        remaining_seconds,
        total_seconds,
        style,
        POMODORO_BREAK_ARC_COLOR
    );
}

/*
 * Action Log RGB565 colours in decimal format.
 * Change only these values to customize the Dynamic Island colours.
 */
static constexpr uint16_t ACTION_LOG_BACKGROUND_COLOR = 0;      // Black
static constexpr uint16_t ACTION_LOG_TEXT_COLOR = 65535;        // White
static constexpr uint16_t ACTION_LOG_BOTTLE_COLOR = 65535;       // Green
static constexpr uint16_t ACTION_LOG_BLUETOOTH_COLOR = 65504;      // Blue
static constexpr uint16_t ACTION_LOG_OTA_COLOR = 2047;          // Cyan
static constexpr uint16_t ACTION_LOG_WATER_COLOR = 65504;        // Cyan
static constexpr uint16_t ACTION_LOG_SHADOW_COLOR = 0;       // Dark shadow
static constexpr uint16_t ACTION_LOG_OUTLINE_COLOR = 0;     // Dark grey-blue
static constexpr uint16_t ACTION_LOG_MUTED_COLOR = 31727;       // Dark grey

static float action_log_smoothstep(float value)
{
    if (value <= 0.0f)
    {
        return 0.0f;
    }

    if (value >= 1.0f)
    {
        return 1.0f;
    }

    return value * value * (3.0f - (2.0f * value));
}

static void draw_action_log_bottle_icon(int center_x, int center_y, uint16_t color)
{
    // Small bottle silhouette designed for the compact island state.
    screen.fillRoundRect(center_x - 4, center_y - 8, 8, 4, 1, color);
    screen.fillRoundRect(center_x - 6, center_y - 5, 12, 14, 3, color);
    screen.fillRect(center_x - 3, center_y - 10, 6, 3, color);

    // Water line/detail.
    screen.drawFastHLine(center_x - 4, center_y + 3, 8, ACTION_LOG_WATER_COLOR);
}

static void draw_action_log_bluetooth_icon(
    int center_x,
    int center_y,
    uint16_t color
)
{
    // Simple Bluetooth rune.
    screen.drawFastVLine(center_x, center_y - 9, 19, color);
    screen.drawLine(center_x, center_y - 9, center_x + 6, center_y - 3, color);
    screen.drawLine(center_x + 6, center_y - 3, center_x - 5, center_y + 5, color);
    screen.drawLine(center_x - 5, center_y - 5, center_x + 6, center_y + 3, color);
    screen.drawLine(center_x + 6, center_y + 3, center_x, center_y + 9, color);
}

static void draw_action_log_overlay()
{
    ActionLogSnapshot snapshot;

    if (!action_log_get_snapshot(snapshot))
    {
        return;
    }

    /*
     * Dynamic-Island-style presentation:
     *   1. starts as a compact black pill,
     *   2. expands smoothly to reveal the message,
     *   3. collapses before a temporary bottle event disappears.
     *
     * This changes only the Action Log graphics. Bottle-clean remains a
     * normal full-screen reminder like Walk, Stretch and Eye Break.
     */
    static constexpr uint32_t ENTER_ANIMATION_MS = 280;
    static constexpr uint32_t EXIT_ANIMATION_MS = 220;

    float expansion = action_log_smoothstep(
        static_cast<float>(snapshot.visible_elapsed_ms) /
        static_cast<float>(ENTER_ANIMATION_MS)
    );

    if (!snapshot.ota_active &&
        snapshot.visible_remaining_ms > 0 &&
        snapshot.visible_remaining_ms < EXIT_ANIMATION_MS)
    {
        const float exit_expansion = action_log_smoothstep(
            static_cast<float>(snapshot.visible_remaining_ms) /
            static_cast<float>(EXIT_ANIMATION_MS)
        );

        if (exit_expansion < expansion)
        {
            expansion = exit_expansion;
        }
    }

    static constexpr int COMPACT_WIDTH = 42;
    static constexpr int EXPANDED_WIDTH = 170;
    static constexpr int COMPACT_HEIGHT = 28;
    static constexpr int EXPANDED_HEIGHT = 42;
    static constexpr int ISLAND_CENTER_X = 120;
    static constexpr int ISLAND_BOTTOM_MARGIN = 30;

    const int island_width = COMPACT_WIDTH + static_cast<int>(
        static_cast<float>(EXPANDED_WIDTH - COMPACT_WIDTH) * expansion
    );
    const int island_height = COMPACT_HEIGHT + static_cast<int>(
        static_cast<float>(EXPANDED_HEIGHT - COMPACT_HEIGHT) * expansion
    );
    const int island_x = ISLAND_CENTER_X - (island_width / 2);
    const int island_y = DISPLAY_HEIGHT - ISLAND_BOTTOM_MARGIN - island_height;
    const int corner_radius = island_height / 2;

    // Soft shadow gives the island separation from the clock background.
    screen.fillRoundRect(
        island_x + 1,
        island_y + 2,
        island_width,
        island_height,
        corner_radius,
        ACTION_LOG_SHADOW_COLOR
    );

    screen.fillRoundRect(
        island_x,
        island_y,
        island_width,
        island_height,
        corner_radius,
        ACTION_LOG_BACKGROUND_COLOR
    );

    uint16_t accent = ACTION_LOG_BOTTLE_COLOR;

    if (snapshot.source == ActionLogSource::BLUETOOTH)
    {
        accent = ACTION_LOG_BLUETOOTH_COLOR;
    }
    else if (snapshot.source == ActionLogSource::OTA)
    {
        accent = ACTION_LOG_OTA_COLOR;
    }

    // A subtle outline appears as the island expands.
    if (expansion > 0.35f)
    {
        screen.drawRoundRect(
            island_x,
            island_y,
            island_width,
            island_height,
            corner_radius,
            ACTION_LOG_OUTLINE_COLOR
        );
    }

    int icon_x = ISLAND_CENTER_X;
    if (expansion > 0.15f)
    {
        icon_x = island_x + 22;
    }
    const int icon_y = island_y + (island_height / 2);

    // Only Bottle and Bluetooth use symbols inside the Action Log.
    // OTA uses text only; its progress is shown by the full-screen arc.
    if (snapshot.source == ActionLogSource::BLUETOOTH)
    {
        draw_action_log_bluetooth_icon(icon_x, icon_y, accent);
    }
    else if (snapshot.source == ActionLogSource::BOTTLE)
    {
        draw_action_log_bottle_icon(icon_x, icon_y, accent);
    }

    // Reveal text only after enough room exists, preventing overlap during entry.
    if (expansion > 0.55f)
    {
        char display_message[30] = {};
        std::snprintf(
            display_message,
            sizeof(display_message),
            "%.27s",
            snapshot.message
        );

        screen.loadFont(font_small);
        screen.setTextDatum(lgfx::textdatum_t::middle_center);
        screen.setTextColor(ACTION_LOG_TEXT_COLOR);
        screen.setTextSize(1);

        int text_center_x = ISLAND_CENTER_X;

        if (snapshot.source == ActionLogSource::BOTTLE ||
            snapshot.source == ActionLogSource::BLUETOOTH)
        {
            const int text_area_left = island_x + 39;
            const int text_area_right = island_x + island_width - 10;
            text_center_x = text_area_left +
                ((text_area_right - text_area_left) / 2);
        }

        screen.drawString(
            display_message,
            text_center_x,
            icon_y
        );
        screen.unloadFont();
    }
}

static void draw_ota_full_screen_progress_arc()
{
    ActionLogSnapshot snapshot;

    if (!action_log_get_snapshot(snapshot) ||
        snapshot.source != ActionLogSource::OTA)
    {
        return;
    }

    static constexpr int ARC_RADIUS = 112;
    static constexpr int ARC_WIDTH = 7;
    static constexpr float ARC_START_ANGLE = 270.0f;

    float arc_start = ARC_START_ANGLE;
    float arc_end = ARC_START_ANGLE;

    if (snapshot.indeterminate)
    {
        arc_start = static_cast<float>(snapshot.animation_phase);
        arc_end = arc_start + 80.0f;
    }
    else if (snapshot.progress_percentage >= 0)
    {
        int percentage = snapshot.progress_percentage;

        if (percentage > 100)
        {
            percentage = 100;
        }

        arc_end = ARC_START_ANGLE +
            (static_cast<float>(percentage) / 100.0f) * 360.0f;
    }

    if (arc_end > arc_start)
    {
        screen.fillArc(
            CLOCK_CENTER_X,
            CLOCK_CENTER_Y,
            ARC_RADIUS,
            ARC_RADIUS + ARC_WIDTH - 1,
            arc_start,
            arc_end,
            ACTION_LOG_OTA_COLOR
        );
    }
}

/* =========================================================
 * Home clock screen
 * ========================================================= */
void display_show_home_clock(time_t current_time)
{
    if (!display_ready || !sprite_ready)
    {
        return;
    }

    struct tm time_info = {};

    localtime_r(
        &current_time,
        &time_info
    );

    // Draw background wallpaper
    screen.setSwapBytes(true);

    screen.pushImage(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        clock_bg_data
    );

    // Blink ":" every second
    char time_text[6];

    if ((time_info.tm_sec % 2) == 0)
    {
        snprintf(
            time_text,
            sizeof(time_text),
            "%02d:%02d",
            time_info.tm_hour,
            time_info.tm_min
        );
    }
    else
    {
        snprintf(
            time_text,
            sizeof(time_text),
            "%02d.%02d",
            time_info.tm_hour,
            time_info.tm_min
        );
    }

    screen.loadFont(font);

    screen.setTextDatum(
        lgfx::textdatum_t::middle_center
    );

    screen.setTextColor(TFT_WHITE);
    screen.setTextSize(1);

    screen.drawString(
        time_text,
        CLOCK_CENTER_X,
        CLOCK_CENTER_Y - 10
    );

    screen.unloadFont();

    draw_ota_full_screen_progress_arc();
    draw_action_log_overlay();

    // Push completed frame
    display.startWrite();
    screen.pushSprite(0, 0);
    display.endWrite();
}

/* =========================================================
 * Bottle calibration full-screen wizard
 * ========================================================= */

/*
 * Draw one calibration text block using font_regular while keeping it inside
 * the usable area of the circular GC9A01 display.
 *
 * The text is wrapped on word boundaries and limited to two lines.  Each
 * block has its own maximum width because the circle is narrower near the
 * top and bottom.
 */
static void draw_calibration_text_block(
    const char* text,
    int16_t center_y,
    uint16_t maximum_width,
    uint16_t color
)
{
    if (text == nullptr || text[0] == '\0')
    {
        return;
    }

    static constexpr size_t BUFFER_SIZE = 96;
    static constexpr int MAX_LINES = 2;

    char source[BUFFER_SIZE] = {};
    char lines[MAX_LINES][BUFFER_SIZE] = {};

    /* Copy with an explicit bound to avoid format-truncation warnings. */
    const size_t source_length = std::strlen(text);
    const size_t source_copy_length =
        source_length < (BUFFER_SIZE - 1)
            ? source_length
            : (BUFFER_SIZE - 1);

    std::memcpy(source, text, source_copy_length);
    source[source_copy_length] = '\0';

    int line_count = 0;
    char* save_pointer = nullptr;
    char* word = strtok_r(source, " ", &save_pointer);

    while (word != nullptr && line_count < MAX_LINES)
    {
        char candidate[BUFFER_SIZE] = {};

        const size_t current_length =
            std::strlen(lines[line_count]);
        const size_t word_length = std::strlen(word);

        size_t candidate_length = 0;

        if (current_length > 0)
        {
            const size_t first_copy =
                current_length < (BUFFER_SIZE - 1)
                    ? current_length
                    : (BUFFER_SIZE - 1);

            std::memcpy(
                candidate,
                lines[line_count],
                first_copy
            );

            candidate_length = first_copy;

            if (candidate_length < (BUFFER_SIZE - 1))
            {
                candidate[candidate_length++] = ' ';
            }
        }

        if (candidate_length < (BUFFER_SIZE - 1))
        {
            const size_t available =
                (BUFFER_SIZE - 1) - candidate_length;
            const size_t word_copy =
                word_length < available
                    ? word_length
                    : available;

            std::memcpy(
                candidate + candidate_length,
                word,
                word_copy
            );

            candidate_length += word_copy;
        }

        candidate[candidate_length] = '\0';

        if (
            screen.textWidth(candidate) <=
            static_cast<int32_t>(maximum_width)
        )
        {
            const size_t copy_length =
                candidate_length < (BUFFER_SIZE - 1)
                    ? candidate_length
                    : (BUFFER_SIZE - 1);

            std::memcpy(
                lines[line_count],
                candidate,
                copy_length
            );

            lines[line_count][copy_length] = '\0';
        }
        else if (lines[line_count][0] == '\0')
        {
            /*
             * A single word is wider than the safe area. Keep it visible by
             * shortening it rather than allowing it to cross the circle.
             */
            const size_t copy_length =
                word_length < (BUFFER_SIZE - 1)
                    ? word_length
                    : (BUFFER_SIZE - 1);

            std::memcpy(
                lines[line_count],
                word,
                copy_length
            );

            lines[line_count][copy_length] = '\0';

            while (
                std::strlen(lines[line_count]) > 1 &&
                screen.textWidth(lines[line_count]) >
                    static_cast<int32_t>(maximum_width)
            )
            {
                lines[line_count][
                    std::strlen(lines[line_count]) - 1
                ] = '\0';
            }

            ++line_count;
        }
        else
        {
            ++line_count;

            if (line_count < MAX_LINES)
            {
                const size_t copy_length =
                    word_length < (BUFFER_SIZE - 1)
                        ? word_length
                        : (BUFFER_SIZE - 1);

                std::memcpy(
                    lines[line_count],
                    word,
                    copy_length
                );

                lines[line_count][copy_length] = '\0';
            }
        }

        word = strtok_r(nullptr, " ", &save_pointer);
    }

    if (
        line_count < MAX_LINES &&
        lines[line_count][0] != '\0'
    )
    {
        ++line_count;
    }

    if (line_count <= 0)
    {
        return;
    }

    int16_t line_height =
        static_cast<int16_t>(screen.fontHeight());

    if (line_height <= 0)
    {
        line_height = 18;
    }

    const int16_t line_spacing = line_height + 2;
    const int16_t first_y =
        center_y -
        static_cast<int16_t>(
            ((line_count - 1) * line_spacing) / 2
        );

    screen.setTextDatum(
        lgfx::textdatum_t::middle_center
    );

    screen.setTextColor(color);
    screen.setTextSize(1);

    for (int index = 0; index < line_count; ++index)
    {
        screen.drawString(
            lines[index],
            DISPLAY_WIDTH / 2,
            first_y + index * line_spacing
        );
    }
}

static void draw_calibration_screen(
    const char* heading,
    const char* line1,
    const char* line2
)
{
    if (!display_ready || !sprite_ready)
    {
        return;
    }

    screen.fillSprite(TFT_BLACK);

    /*
     * Keep font_regular, but wrap each block within a width that is safe for
     * its vertical position on the circular display.
     */
    screen.loadFont(font_regular);
    screen.setTextSize(1);

    draw_calibration_text_block(
        heading != nullptr ? heading : "",
        45,
        145,
        TFT_WHITE
    );

    draw_calibration_text_block(
        line1 != nullptr ? line1 : "",
        108,
        180,
        TFT_WHITE
    );

    draw_calibration_text_block(
        line2 != nullptr ? line2 : "",
        168,
        160,
        TFT_WHITE
    );

    screen.unloadFont();

    screen.drawCircle(
        DISPLAY_WIDTH / 2,
        DISPLAY_HEIGHT / 2,
        112,
        ACTION_LOG_MUTED_COLOR
    );

    display.startWrite();
    screen.pushSprite(0, 0);
    display.endWrite();
}

void display_show_calibration_remove_bottle()
{
    draw_calibration_screen(
        "CALIBRATION",
        "REMOVE BOTTLE",
        "Preparing tare "
    );
}

void display_show_calibration_place_empty()
{
    draw_calibration_screen(
        "CALIBRATION",
        "place empty bottle",
        "Keep it on the dock"
    );
}

void display_show_calibration_fill_bottle()
{
    draw_calibration_screen(
        "CALIBRATION",
        "Remove bottle & fill",
        "it completely"
    );
}

void display_show_calibration_place_full()
{
    draw_calibration_screen(
        "CALIBRATION",
        "Place full bottle",
        "on the dock"
    );
}

void display_show_calibration_measuring(
    const char* title,
    const char* subtitle
)
{
    draw_calibration_screen(
        "MEASURING",
        title,
        subtitle
    );
}

void display_show_calibration_complete(float capacity_ml)
{
    char capacity[48] = {};

    std::snprintf(
        capacity,
        sizeof(capacity),
        "Got_it:%.0f_ml",
        capacity_ml
    );

    draw_calibration_screen(
        "COMPLETE",
        capacity,
        "Tracking is ready"
    );
}

void display_show_calibration_error(const char* message)
{
    draw_calibration_screen(
        "CALIBRATION",
        "ERROR",
        message != nullptr ? message : "Try again"
    );
}


/* =========================================================
 * Separate hydration consumption screen
 * ========================================================= */

void display_show_consumption_screen(
    uint32_t consumed_ml,
    uint32_t daily_consumed_ml,
    uint32_t daily_goal_ml
)
{
    if (!display_ready || !sprite_ready)
    {
        return;
    }

    /*
     * Draw the dedicated 240 x 240 consumption background first.
     * The image header must expose:
     *
     *     hydration_consumption_data
     */
    screen.setSwapBytes(true);

    screen.pushImage(
        0,
        0,
        DISPLAY_WIDTH,
        DISPLAY_HEIGHT,
        hydration_consumption_data
    );

    char consumed_text[24] = {};
    char today_text[40] = {};

    std::snprintf(
        consumed_text,
        sizeof(consumed_text),
        "%lu ml",
        static_cast<unsigned long>(consumed_ml)
    );

    const double daily_consumed_l =
        static_cast<double>(daily_consumed_ml) / 1000.0;

    const double daily_goal_l =
        static_cast<double>(daily_goal_ml) / 1000.0;

    std::snprintf(
        today_text,
        sizeof(today_text),
        "%.1f / %.1f L",
        daily_consumed_l,
        daily_goal_l
    );

    /*
     * Consumption screen layout is intentionally hard-coded so it does not
     * depend on JSON styling or reminder configuration.
     */
    
    static constexpr int16_t CONSUMED_VALUE_X = 120;
    static constexpr int16_t CONSUMED_VALUE_Y = 116;
    
    static constexpr int16_t TODAY_VALUE_X = 120;
    static constexpr int16_t TODAY_VALUE_Y = 185;

    
    static constexpr uint16_t CONSUMED_VALUE_COLOR = 0; // Cyan-blue
        // White
    static constexpr uint16_t TODAY_VALUE_COLOR = 0;    // Yellow

    screen.loadFont(font_regular);
    screen.setTextDatum(lgfx::textdatum_t::middle_center);
    screen.setTextSize(1);

    /* Transparent text keeps the background image visible. */
   

    screen.setTextColor(CONSUMED_VALUE_COLOR);
    screen.drawString(
        consumed_text,
        CONSUMED_VALUE_X,
        CONSUMED_VALUE_Y
    );

    screen.setTextColor(TODAY_VALUE_COLOR);
    screen.drawString(
        today_text,
        TODAY_VALUE_X,
        TODAY_VALUE_Y
    );

    screen.unloadFont();

    display.startWrite();
    screen.pushSprite(0, 0);
    display.endWrite();
}