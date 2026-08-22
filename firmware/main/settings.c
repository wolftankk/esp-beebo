#include <string.h>
#include "settings.h"
#include "board_bsp.h"
#include "ui.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "settings";
#define NVS_NS "ocn"

static settings_t s = {
    .volume     = 70,
    .brightness = 200,
    .font       = 1,
    .doze_sec   = 90,
};

settings_t *settings_get(void) { return &s; }

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_u8(h,  "vol",  &s.volume);
    nvs_get_u8(h,  "bri",  &s.brightness);
    nvs_get_u8(h,  "font", &s.font);
    nvs_get_u16(h, "doze", &s.doze_sec);
    nvs_close(h);
    if (s.brightness < 10) s.brightness = 10;   /* never let it go fully dark */
    if (s.font > 2) s.font = 1;
    ESP_LOGI(TAG, "vol=%u bri=%u font=%u doze=%us",
             s.volume, s.brightness, s.font, s.doze_sec);
}

/* Every +/- tap used to commit to flash. Dozens of taps meant dozens of NVS
 * writes, which burns pages and widens the window where losing power leaves
 * the partition inconsistent. Coalesce instead: the last change within the
 * window is the one that lands. */
#define SAVE_DELAY_US (1500 * 1000)
static esp_timer_handle_t s_save_timer;

static void save_now(void *arg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h,  "vol",  s.volume);
    nvs_set_u8(h,  "bri",  s.brightness);
    nvs_set_u8(h,  "font", s.font);
    nvs_set_u16(h, "doze", s.doze_sec);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings committed (%s)", esp_err_to_name(err));
}

void settings_save(void)
{
    if (!s_save_timer) {
        const esp_timer_create_args_t a = { .callback = save_now, .name = "setsave" };
        if (esp_timer_create(&a, &s_save_timer) != ESP_OK) { save_now(NULL); return; }
    }
    esp_timer_stop(s_save_timer);              /* restart the window */
    esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
}

void settings_save_blocking(void)
{
    save_now(NULL);
}

void settings_apply(void)
{
    bsp_backlight_set(s.brightness);
    ui_apply_settings();
}
