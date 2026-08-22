#include "board_bsp.h"
#include "driver/ledc.h"

#define BL_TIMER    LEDC_TIMER_3
#define BL_CHANNEL  LEDC_CHANNEL_1

void bsp_backlight_init(uint8_t duty)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = BL_TIMER,
        .freq_hz         = 50 * 1000,
        .clk_cfg         = LEDC_SLOW_CLK_RC_FAST,
    };
    ledc_channel_config_t ch_conf = {
        .gpio_num   = BSP_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_TIMER,
        .duty       = duty,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));
    ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));
}

void bsp_backlight_set(uint8_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}
