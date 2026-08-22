idf_component_register(
    SRCS "main.c" "ui.c" "ui_settings.c" "settings.c" "net.c" "batt.c" "audio.c" "expander.c" "voice.c" "probe.c" "imu.c" "fonts/lv_font_pixel_12.c"
    INCLUDE_DIRS "." "fonts"
    REQUIRES board_bsp codec_board nvs_flash esp_wifi esp_netif esp-tls lwip json app_update esp_adc esp_http_client driver fatfs sdmmc esp_driver_sdmmc esp_driver_tsens spi_flash
)

# Deployment settings come from the environment when it provides them, so that
# a public checkout carries placeholders and never somebody's real network.
# See main/beebo_config.h for the precedence rules.
foreach(beebo_var BEEBO_PROXY_URL BEEBO_PROXY_TOKEN BEEBO_WIFI_SSID
                  BEEBO_WIFI_PASS BEEBO_TZ BEEBO_DEVICE_ID)
    if(DEFINED ENV{${beebo_var}} AND NOT "$ENV{${beebo_var}}" STREQUAL "")
        target_compile_definitions(${COMPONENT_LIB}
                                   PRIVATE ${beebo_var}_ENV="$ENV{${beebo_var}}")
        message(STATUS "esp-beebo: ${beebo_var} taken from the environment")
    endif()
endforeach()

