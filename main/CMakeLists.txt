







idf_component_register(
    SRCS
        "main.cpp"
        "configuration_storage.cpp"
        "display.cpp"
         "reminder_engine.cpp"
        "config_parser.cpp"
        "bluetooth.cpp"
        "rtc_ds3231.cpp"
         "pomodoro.cpp"
        "acknowledgement_input.cpp"
        "audio_manager.cpp"
        "dfplayer.cpp"
        "hx711.cpp"
        "scale.cpp"
        "bottle_calibration.cpp"
        "consumption_tracker.cpp"
       "user_statistics.cpp"
       "statistics_history.cpp"
       "action_log.cpp"
        "wifi_manager.cpp"
        "ota_manager.cpp"
        "demo_mode.cpp"
        "onboarding.cpp"

    INCLUDE_DIRS
        "."

    REQUIRES
        esp_driver_spi
        esp_driver_uart
        esp_lcd
        esp_timer
        driver
         bt
        nvs_flash
        spiffs
         esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_https_ota
        app_update
        mbedtls
        espressif__cjson
        qrcode
        
)

target_include_directories(
    ${COMPONENT_LIB}
    PRIVATE
        ../components/LovyanGFX/src
)
# Optional developer-only service screen bypass. Supply a unique 16–64 character
# alphanumeric key via the build environment; omit it in production builds.
if(DEFINED ENV{FROST_SERVICE_DEV_KEY} AND NOT "$ENV{FROST_SERVICE_DEV_KEY}" STREQUAL "")
    set(_frost_service_key "$ENV{FROST_SERVICE_DEV_KEY}")
    string(LENGTH "${_frost_service_key}" _frost_service_key_len)
    message(STATUS "FROST developer key detected: ${_frost_service_key_len} characters")
    if(_frost_service_key_len LESS 16 OR _frost_service_key_len GREATER 64 OR
       NOT _frost_service_key MATCHES "^[A-Za-z0-9]+$")
        message(FATAL_ERROR "FROST_SERVICE_DEV_KEY must be 16 to 64 alphanumeric characters")
    endif()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        FROST_SERVICE_DEV_KEY=\"${_frost_service_key}\")
endif()




