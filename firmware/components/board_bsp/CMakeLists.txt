idf_component_register(
    SRCS "bsp_display.c" "bsp_backlight.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_lcd esp_timer esp_system
)
