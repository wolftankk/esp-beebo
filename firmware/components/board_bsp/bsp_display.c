/*
 * AXS15231B (172x640, QSPI) panel + integrated touch -> LVGL 9.
 *
 * The timing/format constants below are taken from Waveshare's working
 * 10_LVGL_V9_Test example for this exact board; they are not guesses:
 *   - QSPI, SPI mode 3, 40 MHz, cmd bits 32 / param bits 8, no DC line
 *   - panel reset is driven manually on GPIO21 (driver reset pin left at -1)
 *   - a full frame is pushed in 10 DMA chunks, ping-ponged on a semaphore
 */
#include <string.h>
#include "board_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_disp";

#define LVGL_TICK_PERIOD_MS 5
#define LVGL_TASK_MAX_DELAY 500
#define LVGL_TASK_MIN_DELAY 10
#define LVGL_TASK_STACK     (8 * 1024)
#define LVGL_TASK_PRIO      2

static SemaphoreHandle_t s_lvgl_mux;
static SemaphoreHandle_t s_flush_sem;
static uint16_t         *s_dma_buf;
/* A frozen screen with a live system means the LVGL mutex is held by someone
 * who never gave it back. Recording the holder turns that from an audit of
 * every call site into a single log line. */
static const char *s_lock_owner = "(none)";
static uint32_t    s_lock_since;
static int         s_lock_depth;

static i2c_master_bus_handle_t s_touch_bus;
static i2c_master_dev_handle_t s_touch_dev;

/* AXS15231B only needs sleep-out + display-on; the rest is in the driver. */
static const axs15231b_lcd_init_cmd_t s_init_cmds[] = {
    {0x11, (uint8_t []){0x00}, 0, 100},
    {0x29, (uint8_t []){0x00}, 0, 100},
};

static bool on_trans_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *ed, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &hp);
    return hp == pdTRUE;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    lv_draw_sw_rgb565_swap(px, lv_area_get_width(area) * lv_area_get_height(area));

    const int chunks   = BSP_LCD_FRAME_BYTES / BSP_LCD_DMA_BYTES;   /* 10 */
    const int rows     = BSP_LCD_V_RES / chunks;                    /* 64  */
    const int px_chunk = BSP_LCD_DMA_BYTES / 2;
    uint16_t *src = (uint16_t *)px;
    int y1 = 0, y2 = rows;

    /* Prime the ping-pong: first iteration must not block. */
    xSemaphoreGive(s_flush_sem);
    for (int i = 0; i < chunks; i++) {
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        memcpy(s_dma_buf, src, BSP_LCD_DMA_BYTES);
        esp_lcd_panel_draw_bitmap(panel, 0, y1, BSP_LCD_H_RES, y2, s_dma_buf);
        y1 += rows;
        y2 += rows;
        src += px_chunk;
    }
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);   /* wait for the last transfer */
    lv_display_flush_ready(disp);
}

/* AXS15231B touch uses a vendor command, not a register read. */
/* Counters rather than per-read logging: this runs inside LVGL's input loop,
 * where anything chatty can back up the console and stall the UI. */
static uint32_t s_touch_reads, s_touch_fails;

void bsp_touch_stats(uint32_t *reads, uint32_t *fails)
{
    if (reads) *reads = s_touch_reads;
    if (fails) *fails = s_touch_fails;
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static const uint8_t cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x0e, 0, 0, 0};
    uint8_t buf[32] = {0};

    esp_err_t err = i2c_master_transmit_receive(s_touch_dev, cmd, sizeof(cmd),
                                                buf, sizeof(buf), pdMS_TO_TICKS(200));
    if (err != ESP_OK) {
        /* A controller that has stopped answering looks exactly like a frozen
         * screen from the outside, so it must not fail silently. */
        if (++s_touch_fails % 200 == 1)
            ESP_LOGW(TAG, "touch i2c failing: %s (%u failures)",
                     esp_err_to_name(err), (unsigned)s_touch_fails);
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    s_touch_reads++;

    uint16_t px = (((uint16_t)buf[2] & 0x0f) << 8) | buf[3];
    uint16_t py = (((uint16_t)buf[4] & 0x0f) << 8) | buf[5];

    if (buf[1] > 0 && buf[1] < 5) {
        /* Panel reports in its native landscape frame; map to our portrait one. */
        if (px > BSP_LCD_V_RES) px = BSP_LCD_V_RES;
        if (py > BSP_LCD_H_RES) py = BSP_LCD_H_RES;
        data->point.x = py;
        data->point.y = BSP_LCD_V_RES - px;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static void tick_cb(void *arg) { lv_tick_inc(LVGL_TICK_PERIOD_MS); }

bool bsp_lvgl_lock(int timeout_ms)
{
    TickType_t t = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTakeRecursive(s_lvgl_mux, t) != pdTRUE) return false;
    if (++s_lock_depth == 1) {
        s_lock_owner = pcTaskGetName(NULL);
        s_lock_since = xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
    return true;
}

void bsp_lvgl_unlock(void)
{
    if (--s_lock_depth <= 0) {
        s_lock_depth = 0;
        s_lock_owner = "(none)";
    }
    xSemaphoreGiveRecursive(s_lvgl_mux);
}

void bsp_lvgl_lock_info(const char **owner, uint32_t *held_ms, int *depth)
{
    uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
    if (owner)   *owner   = s_lock_owner;
    if (held_ms) *held_ms = s_lock_depth ? now - s_lock_since : 0;
    if (depth)   *depth   = s_lock_depth;
}

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t delay = LVGL_TASK_MAX_DELAY;
        /* Bounded, not portMAX_DELAY. Waiting forever turns any missing unlock
         * anywhere in the codebase into a permanently frozen screen with no
         * clue as to the culprit; timing out names them and keeps running. */
        if (bsp_lvgl_lock(2000)) {
            delay = lv_timer_handler();
            bsp_lvgl_unlock();
        } else {
            ESP_LOGE(TAG, "lvgl lock held by %s for %ums - skipping a frame",
                     s_lock_owner, (unsigned)(xTaskGetTickCount() * portTICK_PERIOD_MS - s_lock_since));
        }
        if (delay > LVGL_TASK_MAX_DELAY) delay = LVGL_TASK_MAX_DELAY;
        if (delay < LVGL_TASK_MIN_DELAY) delay = LVGL_TASK_MIN_DELAY;
        vTaskDelay(pdMS_TO_TICKS(delay));
    }
}

static esp_err_t touch_bus_init(void)
{
    i2c_master_bus_config_t bus = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = BSP_TOUCH_I2C_PORT,
        .scl_io_num                   = BSP_PIN_TOUCH_SCL,
        .sda_io_num                   = BSP_PIN_TOUCH_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &s_touch_bus), TAG, "touch bus");

    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_TOUCH_ADDR,
        .scl_speed_hz    = 300000,
    };
    return i2c_master_bus_add_device(s_touch_bus, &dev, &s_touch_dev);
}

esp_err_t bsp_display_start(void)
{
    s_flush_sem = xSemaphoreCreateBinary();
    s_lvgl_mux  = xSemaphoreCreateRecursiveMutex();
    ESP_RETURN_ON_FALSE(s_flush_sem && s_lvgl_mux, ESP_ERR_NO_MEM, TAG, "sem alloc");

    ESP_RETURN_ON_ERROR(touch_bus_init(), TAG, "touch i2c");

    gpio_config_t rst_cfg = {
        .mode         = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_LCD_RST,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "rst gpio");

    spi_bus_config_t bus = {
        .sclk_io_num    = BSP_PIN_LCD_PCLK,
        .data0_io_num   = BSP_PIN_LCD_D0,
        .data1_io_num   = BSP_PIN_LCD_D1,
        .data2_io_num   = BSP_PIN_LCD_D2,
        .data3_io_num   = BSP_PIN_LCD_D3,
        .max_transfer_sz = BSP_LCD_DMA_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num        = BSP_PIN_LCD_CS,
        .dc_gpio_num        = -1,
        .spi_mode           = 3,
        .pclk_hz            = 40 * 1000 * 1000,
        .trans_queue_depth  = 10,
        .on_color_trans_done = on_trans_done,
        .lcd_cmd_bits       = 32,
        .lcd_param_bits     = 8,
        .flags.quad_mode    = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(BSP_LCD_HOST, &io_cfg, &io), TAG, "panel io");

    axs15231b_vendor_config_t vendor = {
        .flags.use_qspi_interface = 1,
        .init_cmds      = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
    };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,               /* reset pulsed manually below */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config  = &vendor,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(io, &panel_cfg, &panel), TAG, "panel");

    gpio_set_level(BSP_PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
    gpio_set_level(BSP_PIN_LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(250));
    gpio_set_level(BSP_PIN_LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(30));
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "panel init");

    lv_init();
    lv_display_t *disp = lv_display_create(BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_user_data(disp, panel);

    /* Two full frames in PSRAM (~220 KB each) + one DMA-capable chunk in internal RAM. */
    uint8_t *fb1 = heap_caps_malloc(BSP_LCD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    uint8_t *fb2 = heap_caps_malloc(BSP_LCD_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    s_dma_buf    = heap_caps_malloc(BSP_LCD_DMA_BYTES,   MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(fb1 && fb2 && s_dma_buf, ESP_ERR_NO_MEM, TAG, "framebuffer alloc");
    lv_display_set_buffers(disp, fb1, fb2, BSP_LCD_FRAME_BYTES, LV_DISPLAY_RENDER_MODE_FULL);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);

    const esp_timer_create_args_t tick_args = { .callback = tick_cb, .name = "lv_tick" };
    esp_timer_handle_t tick;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick), TAG, "tick timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick, LVGL_TICK_PERIOD_MS * 1000), TAG, "tick start");

    /* Core 1, away from wifi. The wifi stack runs on core 0 at priority 23
     * against LVGL's 2, and during association it starved the UI loop down to
     * about 60% of its normal rate - which is exactly the "unresponsive while
     * it boots, fine once connected" behaviour. */
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL, 1);
    ESP_LOGI(TAG, "display up: %dx%d", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}
