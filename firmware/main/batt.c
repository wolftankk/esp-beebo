/* Battery sense: ADC1 channel 3 (GPIO4) behind a 1:3 divider, per Waveshare's
 * 01_ADC_Test example for this board. */
#include "batt.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

static const char *TAG = "batt";
#define BATT_CHANNEL ADC_CHANNEL_3
#define DIVIDER      3.0f
#define V_EMPTY      3.30f
#define V_FULL       4.20f

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready;

void batt_init(void)
{
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) return;

    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    if (adc_oneshot_config_channel(s_adc, BATT_CHANNEL, &ch) != ESP_OK) return;

    adc_cali_curve_fitting_config_t cal = {
        .unit_id  = ADC_UNIT_1,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) return;

    s_ready = true;
    ESP_LOGI(TAG, "battery sense ready (%.2f V)", batt_volts());
}

float batt_volts(void)
{
    int raw = 0, mv = 0;
    if (!s_ready) return 0;
    if (adc_oneshot_read(s_adc, BATT_CHANNEL, &raw) != ESP_OK) return 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return 0;
    return mv * 0.001f * DIVIDER;
}

int batt_percent(void)
{
    float v = batt_volts();
    if (v <= 0.5f) return -1;                 /* running off USB, no pack */
    int pct = (int)((v - V_EMPTY) / (V_FULL - V_EMPTY) * 100.0f);
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}
