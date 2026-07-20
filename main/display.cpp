#include "display.hpp"
#include "reminder_types.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "LovyanGFX.hpp"

#include "images/frost_logo.h"
#include "images/clock_bg.h"
#include "images/font.h"
#include "images/drinkwater1.h"
#include "images/image_time_to_stretch_inverted.h"
#include "images/rule.h"
#include "images/short_walk.h"
#include "images/medication_background.h"
#include "images/custom_background.h"
#include "images/meditation.h"

static const char *TAG = "FROST_DISPLAY";

/* =========================================================
 * Display configuration
 * ========================================================= */

#define DISPLAY_WIDTH  240
#define DISPLAY_HEIGHT 240

#define DISPLAY_SCLK_PIN GPIO_NUM_14
#define DISPLAY_MOSI_PIN GPIO_NUM_13
#define DISPLAY_MISO_PIN GPIO_NUM_12
#define DISPLAY_DC_PIN   GPIO_NUM_15
#define DISPLAY_CS_PIN   GPIO_NUM_5
#define DISPLAY_RST_PIN  GPIO_NUM_18

/* =========================================================
 * Clock layout
 * ========================================================= */

static constexpr int CLOCK_CENTER_X = 120;
static constexpr int CLOCK_CENTER_Y = 120;

static constexpr int CLOCK_PROGRESS_RADIUS = 112;
static constexpr int CLOCK_PROGRESS_WIDTH = 6;

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

    display.setRotation(0);
    display.setSwapBytes(true);
    display.fillScreen(TFT_BLACK);

    display_ready = true;

    /*
     * Create a full-screen RGB565 sprite.
     * It lets us draw the wallpaper and clock first,
     * then send everything to the display in one push.
     */
    screen.setColorDepth(16);
    screen.setPsram(true);
    screen.setSwapBytes(true);

    if (screen.createSprite(DISPLAY_WIDTH, DISPLAY_HEIGHT) == nullptr)
    {
        ESP_LOGW(
            TAG,
            "Sprite creation failed; logo can display, but clock cannot"
        );

        sprite_ready = false;
        return true;
    }

    sprite_ready = true;

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

    const int16_t line_height =
        static_cast<int16_t>(
            12 * final_text_size
        );

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
     * Draw medicine name over the background.
     */
    draw_configured_reminder_text(
        label,
        text_x,
        text_y,
        text_size,
        text_color,
        text_align,
        text_width
    );

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
     * Draw custom reminder label over the background.
     */
    draw_configured_reminder_text(
        label,
        text_x,
        text_y,
        text_size,
        text_color,
        text_align,
        text_width
    );

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

    // Push completed frame
    display.startWrite();
    screen.pushSprite(0, 0);
    display.endWrite();
}