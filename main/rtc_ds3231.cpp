#include "rtc_ds3231.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "esp_err.h"
#include "esp_log.h"


static const char *TAG = "FROST_RTC";


/* =========================================================
 * DS3231 configuration
 * ========================================================= */

#define DS3231_I2C_PORT       I2C_NUM_0
#define DS3231_SDA_PIN        GPIO_NUM_8
#define DS3231_SCL_PIN        GPIO_NUM_9
#define DS3231_I2C_ADDRESS    0x68
#define DS3231_I2C_FREQUENCY  100000
#define DS3231_TIMEOUT_MS     1000

#define DS3231_REG_TIME       0x00
#define DS3231_REG_STATUS     0x0F
#define DS3231_OSF_BIT        0x80


static i2c_master_bus_handle_t i2c_bus_handle = nullptr;
static i2c_master_dev_handle_t ds3231_handle = nullptr;
static bool rtc_initialized = false;


/* =========================================================
 * RTC structure
 * ========================================================= */

struct rtc_datetime_t
{
    int year;
    int month;
    int day;
    int weekday;
    int hour;
    int minute;
    int second;
};


/* =========================================================
 * BCD helpers
 * ========================================================= */

static uint8_t decimal_to_bcd(int value)
{
    return static_cast<uint8_t>(
        ((value / 10) << 4) |
        (value % 10)
    );
}


static int bcd_to_decimal(uint8_t value)
{
    return ((value >> 4) * 10) +
           (value & 0x0F);
}


/* =========================================================
 * Date validation
 * ========================================================= */

static bool is_leap_year(int year)
{
    return (
        (year % 4 == 0 && year % 100 != 0) ||
        (year % 400 == 0)
    );
}


static int days_in_month(
    int year,
    int month
)
{
    static const int month_days[] =
    {
        31, 28, 31, 30, 31, 30,
        31, 31, 30, 31, 30, 31
    };

    if (month < 1 || month > 12)
    {
        return 0;
    }

    if (month == 2 && is_leap_year(year))
    {
        return 29;
    }

    return month_days[month - 1];
}


static bool datetime_is_valid(
    const rtc_datetime_t &dt
)
{
    if (dt.year < 2000 || dt.year > 2199)
    {
        return false;
    }

    if (dt.month < 1 || dt.month > 12)
    {
        return false;
    }

    if (
        dt.day < 1 ||
        dt.day > days_in_month(dt.year, dt.month)
    )
    {
        return false;
    }

    if (dt.hour < 0 || dt.hour > 23)
    {
        return false;
    }

    if (dt.minute < 0 || dt.minute > 59)
    {
        return false;
    }

    if (dt.second < 0 || dt.second > 59)
    {
        return false;
    }

    return true;
}


/* =========================================================
 * Low-level DS3231 access
 * ========================================================= */

static esp_err_t ds3231_read_registers(
    uint8_t start_register,
    uint8_t *data,
    size_t length
)
{
    if (
        ds3231_handle == nullptr ||
        data == nullptr ||
        length == 0
    )
    {
        return ESP_ERR_INVALID_ARG;
    }

    return i2c_master_transmit_receive(
        ds3231_handle,
        &start_register,
        1,
        data,
        length,
        DS3231_TIMEOUT_MS
    );
}


static esp_err_t ds3231_read_register(
    uint8_t register_address,
    uint8_t *value
)
{
    return ds3231_read_registers(
        register_address,
        value,
        1
    );
}


static esp_err_t ds3231_write_register(
    uint8_t register_address,
    uint8_t value
)
{
    if (ds3231_handle == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t buffer[2];

    buffer[0] = register_address;
    buffer[1] = value;

    return i2c_master_transmit(
        ds3231_handle,
        buffer,
        sizeof(buffer),
        DS3231_TIMEOUT_MS
    );
}


static esp_err_t clear_oscillator_stop_flag(void)
{
    uint8_t status = 0;

    esp_err_t error = ds3231_read_register(
        DS3231_REG_STATUS,
        &status
    );

    if (error != ESP_OK)
    {
        return error;
    }

    status &= static_cast<uint8_t>(
        ~DS3231_OSF_BIT
    );

    return ds3231_write_register(
        DS3231_REG_STATUS,
        status
    );
}


/* =========================================================
 * Read RTC
 * ========================================================= */

static esp_err_t ds3231_read_datetime(
    rtc_datetime_t &datetime
)
{
    uint8_t registers[7] = {0};

    esp_err_t error = ds3231_read_registers(
        DS3231_REG_TIME,
        registers,
        sizeof(registers)
    );

    if (error != ESP_OK)
    {
        return error;
    }

    datetime.second = bcd_to_decimal(
        registers[0] & 0x7F
    );

    datetime.minute = bcd_to_decimal(
        registers[1] & 0x7F
    );

    /*
     * Handle both 12-hour and 24-hour RTC modes.
     */
    if ((registers[2] & 0x40) != 0)
    {
        int hour_12 = bcd_to_decimal(
            registers[2] & 0x1F
        );

        bool is_pm =
            (registers[2] & 0x20) != 0;

        if (hour_12 == 12)
        {
            hour_12 = 0;
        }

        datetime.hour =
            hour_12 + (is_pm ? 12 : 0);
    }
    else
    {
        datetime.hour = bcd_to_decimal(
            registers[2] & 0x3F
        );
    }

    datetime.weekday =
        registers[3] & 0x07;

    datetime.day = bcd_to_decimal(
        registers[4] & 0x3F
    );

    datetime.month = bcd_to_decimal(
        registers[5] & 0x1F
    );

    bool century =
        (registers[5] & 0x80) != 0;

    datetime.year =
        2000 +
        bcd_to_decimal(registers[6]) +
        (century ? 100 : 0);

    if (!datetime_is_valid(datetime))
    {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}


/* =========================================================
 * Write RTC
 * ========================================================= */

static int calculate_weekday(
    const rtc_datetime_t &datetime
)
{
    struct tm time_information;

    memset(
        &time_information,
        0,
        sizeof(time_information)
    );

    time_information.tm_year =
        datetime.year - 1900;

    time_information.tm_mon =
        datetime.month - 1;

    time_information.tm_mday =
        datetime.day;

    time_information.tm_hour = 12;
    time_information.tm_isdst = -1;

    if (
        mktime(&time_information) ==
        static_cast<time_t>(-1)
    )
    {
        return 1;
    }

    /*
     * tm_wday:
     * 0 = Sunday
     *
     * DS3231:
     * 1 = Sunday
     */
    return time_information.tm_wday + 1;
}


static esp_err_t ds3231_write_datetime(
    const rtc_datetime_t &datetime
)
{
    if (!datetime_is_valid(datetime))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (ds3231_handle == nullptr)
    {
        return ESP_ERR_INVALID_STATE;
    }

    int weekday = calculate_weekday(datetime);

    uint8_t buffer[8];

    buffer[0] = DS3231_REG_TIME;
    buffer[1] = decimal_to_bcd(datetime.second);
    buffer[2] = decimal_to_bcd(datetime.minute);
    buffer[3] = decimal_to_bcd(datetime.hour);
    buffer[4] = decimal_to_bcd(weekday);
    buffer[5] = decimal_to_bcd(datetime.day);
    buffer[6] = decimal_to_bcd(datetime.month);

    if (datetime.year >= 2100)
    {
        buffer[6] |= 0x80;
    }

    buffer[7] = decimal_to_bcd(
        datetime.year % 100
    );

    esp_err_t error = i2c_master_transmit(
        ds3231_handle,
        buffer,
        sizeof(buffer),
        DS3231_TIMEOUT_MS
    );

    if (error != ESP_OK)
    {
        return error;
    }

    return clear_oscillator_stop_flag();
}


/* =========================================================
 * Public initialization
 * ========================================================= */

esp_err_t rtc_ds3231_init(void)
{
    if (rtc_initialized)
    {
        return ESP_OK;
    }

    /*
     * India timezone.
     *
     * POSIX timezone signs are reversed:
     * IST-5:30 means UTC+5:30.
     */
    setenv("TZ", "IST-5:30", 1);
    tzset();

    i2c_master_bus_config_t bus_configuration;

    memset(
        &bus_configuration,
        0,
        sizeof(bus_configuration)
    );

    bus_configuration.clk_source =
        I2C_CLK_SRC_DEFAULT;

    bus_configuration.i2c_port =
        DS3231_I2C_PORT;

    bus_configuration.scl_io_num =
        DS3231_SCL_PIN;

    bus_configuration.sda_io_num =
        DS3231_SDA_PIN;

    bus_configuration.glitch_ignore_cnt = 7;

    bus_configuration.flags.enable_internal_pullup =
        true;

    esp_err_t error = i2c_new_master_bus(
        &bus_configuration,
        &i2c_bus_handle
    );

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not create I2C bus: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    error = i2c_master_probe(
        i2c_bus_handle,
        DS3231_I2C_ADDRESS,
        DS3231_TIMEOUT_MS
    );

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "DS3231 not detected at address 0x%02X",
            DS3231_I2C_ADDRESS
        );

        return error;
    }

    i2c_device_config_t device_configuration;

    memset(
        &device_configuration,
        0,
        sizeof(device_configuration)
    );

    device_configuration.dev_addr_length =
        I2C_ADDR_BIT_LEN_7;

    device_configuration.device_address =
        DS3231_I2C_ADDRESS;

    device_configuration.scl_speed_hz =
        DS3231_I2C_FREQUENCY;

    error = i2c_master_bus_add_device(
        i2c_bus_handle,
        &device_configuration,
        &ds3231_handle
    );

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "Could not add DS3231 device: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    rtc_initialized = true;

    ESP_LOGI(
        TAG,
        "DS3231 initialized successfully"
    );

    return ESP_OK;
}


/* =========================================================
 * Synchronize ESP32 from DS3231
 * ========================================================= */

esp_err_t rtc_ds3231_sync_system_time(void)
{
    if (!rtc_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    rtc_datetime_t rtc;

    esp_err_t error =
        ds3231_read_datetime(rtc);

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "DS3231 read failed: %s",
            esp_err_to_name(error)
        );

        return error;
    }

    struct tm time_information;

    memset(
        &time_information,
        0,
        sizeof(time_information)
    );

    time_information.tm_year =
        rtc.year - 1900;

    time_information.tm_mon =
        rtc.month - 1;

    time_information.tm_mday =
        rtc.day;

    time_information.tm_hour =
        rtc.hour;

    time_information.tm_min =
        rtc.minute;

    time_information.tm_sec =
        rtc.second;

    time_information.tm_isdst = -1;

    time_t unix_time =
        mktime(&time_information);

    if (unix_time == static_cast<time_t>(-1))
    {
        ESP_LOGE(TAG, "mktime failed");
        return ESP_FAIL;
    }

    struct timeval time_value;

    time_value.tv_sec = unix_time;
    time_value.tv_usec = 0;

    if (settimeofday(&time_value, nullptr) != 0)
    {
        ESP_LOGE(
            TAG,
            "settimeofday failed"
        );

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "System time synchronized: "
        "%04d-%02d-%02d %02d:%02d:%02d",
        rtc.year,
        rtc.month,
        rtc.day,
        rtc.hour,
        rtc.minute,
        rtc.second
    );

    return ESP_OK;
}


/* =========================================================
 * Process SET command
 * ========================================================= */

bool rtc_ds3231_process_set_command(
    const char *command
)
{
    if (
        !rtc_initialized ||
        command == nullptr
    )
    {
        return false;
    }

    rtc_datetime_t datetime;

    memset(
        &datetime,
        0,
        sizeof(datetime)
    );

    int parsed_fields = sscanf(
        command,
        "SET %d-%d-%d %d:%d:%d",
        &datetime.year,
        &datetime.month,
        &datetime.day,
        &datetime.hour,
        &datetime.minute,
        &datetime.second
    );

    if (parsed_fields != 6)
    {
        ESP_LOGE(
            TAG,
            "Invalid time command"
        );

        ESP_LOGE(
            TAG,
            "Use: SET YYYY-MM-DD HH:MM:SS"
        );

        return false;
    }

    if (!datetime_is_valid(datetime))
    {
        ESP_LOGE(
            TAG,
            "Invalid date or time values"
        );

        return false;
    }

    esp_err_t error =
        ds3231_write_datetime(datetime);

    if (error != ESP_OK)
    {
        ESP_LOGE(
            TAG,
            "DS3231 write failed: %s",
            esp_err_to_name(error)
        );

        return false;
    }

    error =
        rtc_ds3231_sync_system_time();

    if (error != ESP_OK)
    {
        return false;
    }

    ESP_LOGI(
        TAG,
        "RTC updated through BLE: "
        "%04d-%02d-%02d %02d:%02d:%02d",
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second
    );

    return true;
}


/* =========================================================
 * Get formatted RTC time
 * ========================================================= */

bool rtc_ds3231_get_time_string(
    char *output,
    size_t output_size
)
{
    if (
        output == nullptr ||
        output_size == 0 ||
        !rtc_initialized
    )
    {
        return false;
    }

    rtc_datetime_t rtc;

    if (
        ds3231_read_datetime(rtc) !=
        ESP_OK
    )
    {
        return false;
    }

    int written = snprintf(
        output,
        output_size,
        "%04d-%02d-%02d %02d:%02d:%02d",
        rtc.year,
        rtc.month,
        rtc.day,
        rtc.hour,
        rtc.minute,
        rtc.second
    );

    return (
        written >= 0 &&
        static_cast<size_t>(written) <
            output_size
    );
}