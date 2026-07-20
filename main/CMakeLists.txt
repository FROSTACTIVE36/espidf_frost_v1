idf_component_register(
    SRCS
        "main.cpp"
        "display.cpp"
         "reminder_engine.cpp"
        "config_parser.cpp"
        "bluetooth.cpp"
        "rtc_ds3231.cpp"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_driver_spi
        esp_lcd
        esp_timer
        driver
         bt
        nvs_flash
        
)

target_include_directories(
    ${COMPONENT_LIB}
    PRIVATE
        ../components/LovyanGFX/src
)