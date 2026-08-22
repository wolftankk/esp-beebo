#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board_pins.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Backlight (LEDC PWM on GPIO8), duty 0..255 */
void      bsp_backlight_init(uint8_t duty);
void      bsp_backlight_set(uint8_t duty);

/* Brings up: touch I2C bus, QSPI bus, AXS15231B panel, LVGL display + touch indev.
 * Starts the LVGL timer task. Call once from app_main. */
esp_err_t bsp_display_start(void);

/* LVGL is not thread-safe: take this around every lv_* call made off the LVGL task. */
/* Touch health: a wedged controller is indistinguishable from a frozen UI
 * unless the read failures are counted somewhere visible. */
void      bsp_touch_stats(uint32_t *reads, uint32_t *fails);

bool      bsp_lvgl_lock(int timeout_ms);
/* Who currently holds the LVGL lock, and for how long. */
void      bsp_lvgl_lock_info(const char **owner, uint32_t *held_ms, int *depth);
void      bsp_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
