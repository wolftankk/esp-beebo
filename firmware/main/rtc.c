/*
 * PCF85063A. Stores UTC; the TZ environment variable turns it into wall time,
 * so a timezone change needs no write to the chip.
 *
 * The oscillator-stop flag in the seconds register is the whole reliability
 * story here: it latches whenever the chip has lost track of time, and it stays
 * latched until something writes a real time in. Reading it is how the board
 * distinguishes "the RTC says 2000-01-01 because that is genuinely what it
 * thinks" from "the RTC has never been set", which otherwise look identical and
 * would have it confidently displaying a wrong clock.
 */
#define _GNU_SOURCE
#include <string.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "board_bsp.h"
#include "codec_init.h"
#include "rtc.h"

static const char *TAG = "rtc";

#define REG_CTRL1    0x00
#define REG_SECONDS  0x04       /* bit 7: oscillator stopped */
#define OS_FLAG      0x80

static i2c_master_dev_handle_t s_dev;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, pdMS_TO_TICKS(100));
}

static esp_err_t wr(uint8_t reg, const uint8_t *buf, size_t n)
{
    uint8_t tmp[10];
    if (n + 1 > sizeof(tmp)) return ESP_ERR_INVALID_SIZE;
    tmp[0] = reg;
    memcpy(tmp + 1, buf, n);
    return i2c_master_transmit(s_dev, tmp, n + 1, pdMS_TO_TICKS(100));
}

/* newlib has no timegm, and the alternative - swapping TZ to UTC, calling
 * mktime, swapping it back - is a global side effect on a shared setting for
 * the sake of one arithmetic conversion. Days-from-civil is exact and has no
 * such reach. */
static time_t tm_to_utc(const struct tm *t)
{
    int y = t->tm_year + 1900;
    unsigned m = t->tm_mon + 1, d = t->tm_mday;
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = (long)era * 146097 + (long)doe - 719468;
    return (time_t)days * 86400 + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static int  from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t to_bcd(int v)    { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

bool rtc_present(void) { return s_dev != NULL; }

esp_err_t rtc_start(void)
{
    i2c_master_bus_handle_t bus = (i2c_master_bus_handle_t)get_i2c_bus_handle(0);
    if (!bus) {
        ESP_LOGW(TAG, "system I2C not up - no clock");
        return ESP_ERR_INVALID_STATE;
    }

    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_ADDR_RTC,
        .scl_speed_hz    = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dc, &s_dev) != ESP_OK) return ESP_FAIL;

    uint8_t ctrl;
    if (rd(REG_CTRL1, &ctrl, 1) != ESP_OK) {
        ESP_LOGW(TAG, "no PCF85063 at 0x%02X", BSP_ADDR_RTC);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* Clear STOP so it is counting, and make sure it is in 24-hour mode - the
     * conversion below has no notion of AM/PM. */
    uint8_t want = ctrl & ~((1 << 5) | (1 << 1));
    if (want != ctrl) wr(REG_CTRL1, &want, 1);

    ESP_LOGI(TAG, "PCF85063 present (ctrl1 0x%02X)", want);
    return ESP_OK;
}

bool rtc_restore_system_time(void)
{
    if (!s_dev) return false;

    uint8_t r[7];
    if (rd(REG_SECONDS, r, sizeof(r)) != ESP_OK) return false;

    if (r[0] & OS_FLAG) {
        ESP_LOGW(TAG, "never set (oscillator-stop latched) - waiting for NTP");
        return false;
    }

    struct tm tm = {
        .tm_sec  = from_bcd(r[0] & 0x7F),
        .tm_min  = from_bcd(r[1] & 0x7F),
        .tm_hour = from_bcd(r[2] & 0x3F),
        .tm_mday = from_bcd(r[3] & 0x3F),
        .tm_mon  = from_bcd(r[5] & 0x1F) - 1,
        .tm_year = from_bcd(r[6]) + 100,        /* the chip counts from 2000 */
    };

    /* Converted as UTC, not local: that is what the chip holds. */
    time_t utc = tm_to_utc(&tm);
    if (utc < 1735689600) {                     /* before 2025: not a real time */
        ESP_LOGW(TAG, "stored time looks wrong (%lld) - ignoring", (long long)utc);
        return false;
    }

    struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    char buf[32];
    struct tm local;
    localtime_r(&utc, &local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &local);
    ESP_LOGI(TAG, "restored %s local, before any network", buf);
    return true;
}

esp_err_t rtc_save_system_time(void)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    if (tm.tm_year + 1900 < 2025) return ESP_ERR_INVALID_STATE;

    uint8_t r[7] = {
        to_bcd(tm.tm_sec) & 0x7F,               /* writing seconds clears OS */
        to_bcd(tm.tm_min),
        to_bcd(tm.tm_hour),
        to_bcd(tm.tm_mday),
        to_bcd(tm.tm_wday),
        to_bcd(tm.tm_mon + 1),
        to_bcd(tm.tm_year - 100),
    };
    esp_err_t err = wr(REG_SECONDS, r, sizeof(r));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "saved %04d-%02d-%02d %02d:%02d:%02d UTC",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return ESP_OK;
}
