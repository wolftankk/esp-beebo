/*
 * QMI8658 six-axis, used for one thing: making the robot notice it is being
 * handled.
 *
 * The gestures here are deliberately the three that need no calibration and no
 * threshold tuning per unit - lean, shake, and being set face-down - because
 * anything subtler would fire on a passing lorry and the failure mode of a
 * twitchy pet is worse than one that ignores you.
 *
 * Orientation, measured on this board rather than assumed: standing upright,
 * gravity reads on -Y (accel Y approx -1g) and +Z points out of the *back*, so
 * lying screen-down puts gravity on +Z.
 */
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "board_bsp.h"
#include "codec_init.h"
#include "imu.h"
#include "ui.h"
#include "audio.h"

static const char *TAG = "imu";

#define QMI_WHO_AM_I  0x00
#define QMI_CTRL1     0x02
#define QMI_CTRL2     0x03
#define QMI_CTRL3     0x04
#define QMI_CTRL7     0x08
#define QMI_AX_L      0x35

/* Accel +-8g -> 4096 LSB/g, gyro +-1024dps -> 32 LSB/dps. */
#define ACC_LSB   4096.0f
#define GYR_LSB     32.0f

#define POLL_MS        50        /* 20 Hz: plenty for gravity, cheap enough  */
#define SHAKE_DPS     420.0f     /* a deliberate shake, not a nudge          */
#define SHAKE_HITS       3       /* sustained over ~150ms, so knocks are out */
#define SHAKE_HOLD_MS 4000       /* one dizzy spell per shake, not a stream  */
#define MOVE_G        0.18f      /* picked up, as opposed to breathed on     */
#define FLAT_G        0.88f
#define UPRIGHT_G     0.45f      /* below this the board is not standing up  */
#define FLAT_HOLD_MS  1500       /* face-down has to be settled, not passing */

static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int  s_roll;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, pdMS_TO_TICKS(100));
}

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, 2, pdMS_TO_TICKS(100));
}

bool imu_present(void) { return s_present; }
int  imu_roll_deg(void) { return s_roll; }

static void imu_task(void *arg)
{
    int   shake_run = 0;
    int64_t shake_until = 0, flat_since = 0;
    bool  face_down = false;
    float roll_f = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint8_t raw[12];
        if (rd(QMI_AX_L, raw, 12) != ESP_OK) continue;

        int16_t v[6];
        for (int k = 0; k < 6; k++) v[k] = (int16_t)(raw[k*2] | (raw[k*2+1] << 8));
        float ax = v[0] / ACC_LSB, ay = v[1] / ACC_LSB, az = v[2] / ACC_LSB;
        float gx = v[3] / GYR_LSB, gy = v[4] / GYR_LSB, gz = v[5] / GYR_LSB;

        int64_t now = esp_timer_get_time() / 1000;

        /* ---- lean ---- */
        /* Upright is gravity on -Y, so the lean is X against that - but only
         * while the board actually is upright. Lay it back in a cradle and
         * gravity moves to Z, at which point atan2(X, Y) is two small noisy
         * numbers and the head lurches to the clamp. A board that is not
         * standing up has no lean, so it returns to centre instead. */
        float target = 0;
        if (-ay > UPRIGHT_G) {
            target = atan2f(ax, -ay) * 57.2958f;
            if (target >  45) target =  45;
            if (target < -45) target = -45;
        }
        roll_f += (target - roll_f) * 0.15f;
        s_roll = (int)roll_f;

        /* ---- shake ---- */
        float spin = fabsf(gx) + fabsf(gy) + fabsf(gz);
        if (spin > SHAKE_DPS) {
            if (++shake_run >= SHAKE_HITS && now > shake_until) {
                shake_until = now + SHAKE_HOLD_MS;
                shake_run = 0;
                ESP_LOGI(TAG, "shaken (%.0f dps) - dizzy", spin);
                ui_notice_activity();
                ui_shake();
            }
        } else if (shake_run > 0) {
            shake_run--;
        }

        /* ---- set down screen-first, or picked back up ---- */
        /* Screen-down is gravity almost entirely on +Z. Requiring Y to have
         * gone quiet as well is what separates "put down on the desk" from
         * "propped back in a stand", which otherwise both read az ~ +0.9. */
        bool flat_now = az > FLAT_G && fabsf(ay) < 0.35f;
        if (flat_now) {
            if (!flat_since) flat_since = now;
            if (!face_down && now - flat_since > FLAT_HOLD_MS) {
                face_down = true;
                ESP_LOGI(TAG, "face down - going quiet");
                ui_face_down(true);
            }
        } else {
            flat_since = 0;
            if (face_down) {
                face_down = false;
                ESP_LOGI(TAG, "picked back up");
                ui_face_down(false);
            }
        }

        /* ---- handled at all ---- */
        /* Total acceleration departing from 1g means the board is moving,
         * which is a better wake signal than a touch: you pick it up before
         * you think to prod it. */
        if (!face_down) {
            float mag = sqrtf(ax*ax + ay*ay + az*az);
            if (fabsf(mag - 1.0f) > MOVE_G || spin > 120.0f) ui_notice_activity();
        }

        if (!ui_screen_is_off()) ui_set_tilt(s_roll);
    }
}

esp_err_t imu_start(void)
{
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)get_i2c_bus_handle(0);
    if (!bus) {
        ESP_LOGW(TAG, "system I2C not up - no IMU");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_ADDR_IMU,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) return ESP_FAIL;

    uint8_t id = 0;
    if (rd(QMI_WHO_AM_I, &id, 1) != ESP_OK || id != 0x05) {
        ESP_LOGW(TAG, "no QMI8658 (read 0x%02X)", id);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* Ranges before CTRL7: the sensors latch their scale when they start. */
    wr(QMI_CTRL1, 0x60);        /* register auto-increment on reads */
    wr(QMI_CTRL2, 0x24);        /* accel +-8g                       */
    wr(QMI_CTRL3, 0x64);        /* gyro  +-1024 dps                 */
    wr(QMI_CTRL7, 0x03);        /* both running                     */
    vTaskDelay(pdMS_TO_TICKS(50));

    s_present = true;
    /* Core 1 alongside LVGL: it feeds the face and nothing else, and core 0
     * is where the wifi stack already lives. */
    xTaskCreatePinnedToCore(imu_task, "imu", 3072, NULL, 2, NULL, 1);
    ESP_LOGI(TAG, "QMI8658 running");
    return ESP_OK;
}
