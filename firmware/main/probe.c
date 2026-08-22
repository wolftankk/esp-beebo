/*
 * One-shot hardware census.
 *
 * Everything here asks the silicon rather than trusting a wiki page: the
 * board's product page lists parts, but which of them are actually populated,
 * responding, and reachable from *this* firmware is a different question. Each
 * check prints what it found so a missing part reads as "no answer at 0x6B"
 * instead of a feature that quietly never worked.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/temperature_sensor.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "board_bsp.h"
#include "codec_init.h"
#include "probe.h"
#include "imu.h"

static const char *TAG = "probe";

/* ------------------------------------------------------------------ I2C ---- */

static const struct { uint8_t addr; const char *what; } known[] = {
    { 0x18, "ES8311 codec (speaker out)" },
    { 0x20, "TCA9554 GPIO expander" },
    { 0x3B, "AXS15231B touch" },
    { 0x40, "ES7210 ADC (mic array)" },
    { 0x51, "PCF85063 RTC" },
    { 0x6B, "QMI8658 IMU" },
};

static const char *name_for(uint8_t addr)
{
    for (size_t i = 0; i < sizeof(known)/sizeof(known[0]); i++)
        if (known[i].addr == addr) return known[i].what;
    return "unknown";
}

static void scan(i2c_master_bus_handle_t bus, const char *label)
{
    ESP_LOGI(TAG, "--- %s ---", label);
    int found = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X  %s", a, name_for(a));
            found++;
        }
    }
    if (!found) ESP_LOGW(TAG, "  nothing answered");
}

static esp_err_t dev_open(i2c_master_bus_handle_t bus, uint8_t addr,
                          i2c_master_dev_handle_t *out)
{
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = addr,
        .scl_speed_hz    = 100000,
    };
    return i2c_master_bus_add_device(bus, &dc, out);
}

static esp_err_t rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(d, &reg, 1, buf, n, pdMS_TO_TICKS(100));
}

static esp_err_t wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(d, b, 2, pdMS_TO_TICKS(100));
}

/* ------------------------------------------------------------------ IMU ---- */
/* QMI8658C: 6-axis, accelerometer + gyroscope + its own die temperature. */

#define QMI_WHO_AM_I  0x00
#define QMI_REVISION  0x01
#define QMI_CTRL1     0x02
#define QMI_CTRL2     0x03   /* accel range + rate */
#define QMI_CTRL3     0x04   /* gyro  range + rate */
#define QMI_CTRL7     0x08   /* which sensors run  */
#define QMI_STATUS0   0x2E
#define QMI_TEMP_L    0x33
#define QMI_AX_L      0x35

static void probe_imu(i2c_master_bus_handle_t bus)
{
    /* imu.c already owns 0x6B by now and has it streaming; opening a second
     * handle on the same address would only report on a sensor someone else
     * is configuring. */
    if (imu_present()) {
        ESP_LOGI(TAG, "IMU: QMI8658 running, lean %+d deg", imu_roll_deg());
        return;
    }

    i2c_master_dev_handle_t d;
    if (dev_open(bus, BSP_ADDR_IMU, &d) != ESP_OK) return;

    uint8_t id = 0, rev = 0;
    if (rd(d, QMI_WHO_AM_I, &id, 1) != ESP_OK || id != 0x05) {
        ESP_LOGW(TAG, "IMU: no QMI8658 at 0x%02X (read 0x%02X)", BSP_ADDR_IMU, id);
        goto out;
    }
    rd(d, QMI_REVISION, &rev, 1);
    ESP_LOGI(TAG, "IMU: QMI8658 present, WHO_AM_I=0x%02X revision=0x%02X", id, rev);

    /* Auto-increment on reads, then accel ±8g and gyro ±1024dps at ~235Hz.
     * CTRL7 last: the ranges have to be set before the sensors start. */
    wr(d, QMI_CTRL1, 0x60);
    wr(d, QMI_CTRL2, 0x24);
    wr(d, QMI_CTRL3, 0x64);
    wr(d, QMI_CTRL7, 0x03);
    vTaskDelay(pdMS_TO_TICKS(100));

    uint8_t st = 0;
    rd(d, QMI_STATUS0, &st, 1);
    ESP_LOGI(TAG, "IMU: STATUS0=0x%02X (accel-ready=%d gyro-ready=%d)",
             st, st & 1, (st >> 1) & 1);

    for (int i = 0; i < 5; i++) {
        uint8_t t[2], raw[12];
        if (rd(d, QMI_TEMP_L, t, 2) != ESP_OK) break;
        if (rd(d, QMI_AX_L, raw, 12) != ESP_OK) break;

        float die = (float)(int16_t)(t[0] | (t[1] << 8)) / 256.0f;
        int16_t v[6];
        for (int k = 0; k < 6; k++) v[k] = (int16_t)(raw[k*2] | (raw[k*2+1] << 8));

        /* 4096 LSB/g at ±8g, 32 LSB/dps at ±1024dps. */
        ESP_LOGI(TAG, "IMU: accel %+6.2f %+6.2f %+6.2f g   gyro %+8.1f %+8.1f %+8.1f dps   die %.1fC",
                 v[0]/4096.0f, v[1]/4096.0f, v[2]/4096.0f,
                 v[3]/32.0f,   v[4]/32.0f,   v[5]/32.0f, die);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Left running: a live IMU is the point of finding one. */
out:
    i2c_master_bus_rm_device(d);
}

/* ------------------------------------------------------------------ RTC ---- */
/* PCF85063: keeps time across a power cut on its own backup cell. Its value is
 * that it survives what NTP cannot - no network at boot, and no wall clock. */

static int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

static void probe_rtc(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t d;
    if (dev_open(bus, BSP_ADDR_RTC, &d) != ESP_OK) return;

    uint8_t r[7];
    if (rd(d, 0x04, r, 7) != ESP_OK) {
        ESP_LOGW(TAG, "RTC: no answer at 0x%02X", BSP_ADDR_RTC);
        goto out;
    }

    bool stopped = r[0] & 0x80;          /* oscillator-stop: time is not trustworthy */
    ESP_LOGI(TAG, "RTC: PCF85063 reads 20%02d-%02d-%02d %02d:%02d:%02d%s",
             bcd(r[6]), bcd(r[5] & 0x1F), bcd(r[3] & 0x3F),
             bcd(r[2] & 0x3F), bcd(r[1] & 0x7F), bcd(r[0] & 0x7F),
             stopped ? "  [OSCILLATOR STOPPED - never set / backup cell flat]" : "");

    /* Ticking is the actual question; a frozen register reads fine once. */
    uint8_t s1 = r[0] & 0x7F;
    vTaskDelay(pdMS_TO_TICKS(1200));
    uint8_t s2 = 0;
    rd(d, 0x04, &s2, 1);
    ESP_LOGI(TAG, "RTC: %s (seconds %02d -> %02d)",
             (s2 & 0x7F) != s1 ? "ticking" : "NOT ticking", bcd(s1), bcd(s2 & 0x7F));
out:
    i2c_master_bus_rm_device(d);
}

/* ----------------------------------------------------------------- card ---- */

static void probe_sd(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags    = SDMMC_HOST_FLAG_1BIT;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk   = BSP_PIN_SD_CLK;
    slot.cmd   = BSP_PIN_SD_CMD;
    slot.d0    = BSP_PIN_SD_D0;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files              = 3,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_card_t *card = NULL;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mcfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD: nothing mounted (%s) - slot is wired, card likely absent",
                 esp_err_to_name(err));
        return;
    }

    uint64_t bytes = (uint64_t)card->csd.capacity * card->csd.sector_size;
    ESP_LOGI(TAG, "SD: %s \"%s\" %.2f GB, %d-bit, %d kHz",
             (card->ocr & (1 << 30)) ? "SDHC/SDXC" : "SDSC",
             card->cid.name, bytes / 1e9, 1, card->max_freq_khz);

    FATFS *fs; DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
        uint64_t total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
        uint64_t freeb = (uint64_t)free_clusters * fs->csize * 512;
        ESP_LOGI(TAG, "SD: FAT %llu MB total, %llu MB free",
                 total / 1000000, freeb / 1000000);
    }
    esp_vfs_fat_sdcard_unmount("/sdcard", card);
}

/* ------------------------------------------------------------------ SoC ---- */

static void probe_soc(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);

    char feat[128] = "";
    if (ci.features & CHIP_FEATURE_WIFI_BGN) strcat(feat, "wifi-b/g/n ");
    if (ci.features & CHIP_FEATURE_BLE)      strcat(feat, "BLE ");
    if (ci.features & CHIP_FEATURE_BT)       strcat(feat, "BT-classic ");
    if (ci.features & CHIP_FEATURE_EMB_FLASH)strcat(feat, "embedded-flash ");
    if (ci.features & CHIP_FEATURE_EMB_PSRAM)strcat(feat, "embedded-psram ");

    uint32_t fsize = 0;
    esp_flash_get_size(NULL, &fsize);

    ESP_LOGI(TAG, "SoC: ESP32-S3 rev v%d.%d, %d core%s, %s",
             ci.revision / 100, ci.revision % 100,
             ci.cores, ci.cores > 1 ? "s" : "", feat);
    ESP_LOGI(TAG, "SoC: %lu MB flash, PSRAM %u KB total / %u KB free, internal heap %u KB free",
             (unsigned long)(fsize / (1024*1024)),
             (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));

    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK)
        ESP_LOGI(TAG, "SoC: wifi MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (esp_read_mac(mac, ESP_MAC_BT) == ESP_OK)
        ESP_LOGI(TAG, "SoC: BLE  MAC %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* The die sensor is not a room thermometer - it reads high by however much
     * the chip is working - but it is free and it trends. */
    temperature_sensor_handle_t ts = NULL;
    temperature_sensor_config_t tcfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&tcfg, &ts) == ESP_OK) {
        float c = 0;
        if (temperature_sensor_enable(ts) == ESP_OK &&
            temperature_sensor_get_celsius(ts, &c) == ESP_OK)
            ESP_LOGI(TAG, "SoC: die temperature %.1f C", c);
        temperature_sensor_disable(ts);
        temperature_sensor_uninstall(ts);
    }
}

/* ---------------------------------------------------------------- driver ---- */

static void probe_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));      /* let the noisy part of boot finish */

    ESP_LOGI(TAG, "======== hardware census ========");
    probe_soc();

    i2c_master_bus_handle_t sys = (i2c_master_bus_handle_t)get_i2c_bus_handle(0);
    if (sys) {
        scan(sys, "system I2C (GPIO47/48)");
        probe_imu(sys);
        probe_rtc(sys);
    } else {
        ESP_LOGW(TAG, "system I2C bus not up - codec never initialised?");
    }

    /* The touch controller sits on its own bus, already owned by the display
     * driver, so it is scanned rather than opened. */
    probe_sd();
    ESP_LOGI(TAG, "======== census complete ========");
    vTaskDelete(NULL);
}

void probe_start(void)
{
    xTaskCreate(probe_task, "probe", 5120, NULL, 2, NULL);
}
