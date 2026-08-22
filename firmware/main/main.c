/*
 * OpenClaw node - Waveshare ESP32-S3-Touch-LCD-3.49
 *
 * Stage 1 brings the whole chain up end to end so every layer is proven
 * before audio goes in: display -> settings -> wifi -> clock -> gateway.
 * The BOOT key already drives the robot's mood; stage 2 hangs the mic on it.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "board_bsp.h"
#include "ui.h"
#include "ui_settings.h"
#include "settings.h"
#include "net.h"
#include "batt.h"
#include "audio.h"
#include "expander.h"
#include "voice.h"
#include "probe.h"
#include "imu.h"
#include <math.h>

static const char *TAG = "main";

/* Power button, active low:
 *   tap        -> screen off / on
 *   hold 5 s   -> shut down
 *
 * Shutting down means dropping SYS_EN, which opens the latch the schematic
 * puts between VBAT and VSYS - genuinely off, on battery. On USB the supply
 * comes in past that latch, so nothing would happen; deep sleep with the same
 * button as the wake source gives the identical "press it to come back"
 * behaviour there. */
#define PWR_TAP_MAX_MS   800
#define PWR_SHUTDOWN_MS  5000

static void power_off(void)
{
    ESP_LOGW(TAG, "powering down");
    ui_say("bye");
    audio_sound(SND_SLEEP);         /* winding down, before the amp goes */
    bsp_backlight_set(0);
    audio_amp_enable(false);
    vTaskDelay(pdMS_TO_TICKS(150));

    expander_set(EXIO_SYS_EN, false);          /* cuts battery power outright */
    vTaskDelay(pdMS_TO_TICKS(600));

    /* Still here: running off USB, where the latch is bypassed. */
    ESP_LOGW(TAG, "still powered (USB) - entering deep sleep, press PWR to wake");
    esp_sleep_enable_ext0_wakeup(BSP_PIN_BTN_PWR, 0);
    esp_deep_sleep_start();
}

static void pwr_task(void *arg)
{
    gpio_config_t cfg = {
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_BTN_PWR,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    bool held = false;
    int  stable = 0;
    uint32_t pressed_at = 0;
    bool shutdown_armed = false;

    for (;;) {
        uint32_t now  = xTaskGetTickCount() * portTICK_PERIOD_MS;
        bool     down = gpio_get_level(BSP_PIN_BTN_PWR) == 0;

        stable = (down == held) ? 0 : stable + 1;
        if (stable >= 3) {                       /* ~30 ms debounce */
            held = down;
            stable = 0;
            if (held) {
                pressed_at = now;
                shutdown_armed = false;
            } else if (now - pressed_at <= PWR_TAP_MAX_MS && !shutdown_armed) {
                if (ui_screen_is_off()) ui_screen_on();
                else                    ui_screen_off();
            }
        }

        if (held && !shutdown_armed && now - pressed_at >= PWR_SHUTDOWN_MS) {
            shutdown_armed = true;               /* only once per press */
            power_off();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* BOOT (GPIO0, active low) carries two gestures:
 *   hold        -> push to talk, listening for as long as it is down
 *   double tap  -> escape back to the robot from any settings screen
 * Safe to use at runtime: GPIO0 is only sampled by the ROM bootloader while
 * RESET is being released. */
#define PTT_HOLD_MS    400      /* past this, a press is a hold, not a tap */
#define PTT_DOUBLE_MS  450      /* max gap between the two taps */

static void ptt_task(void *arg)
{
    gpio_config_t cfg = {
        .mode         = GPIO_MODE_INPUT,
        .pin_bit_mask = 1ULL << BSP_PIN_BTN_BOOT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    bool held = false, listening = false;
    int  stable = 0;
    uint32_t pressed_at = 0, last_tap = 0;

    for (;;) {
        uint32_t now  = xTaskGetTickCount() * portTICK_PERIOD_MS;
        bool     down = gpio_get_level(BSP_PIN_BTN_BOOT) == 0;

        stable = (down == held) ? 0 : stable + 1;
        if (stable >= 3) {                          /* ~30 ms debounce */
            held   = down;
            stable = 0;

            if (held) {
                pressed_at = now;
                ui_notice_activity();
            } else {
                if (listening) {                    /* end of a hold */
                    listening = false;
                    voice_stop();                   /* uploads and answers */
                } else if (now - last_tap <= PTT_DOUBLE_MS) {
                    ui_settings_close_all();        /* second tap of a pair */
                    ui_set_mood(MOOD_HAPPY);
                    last_tap = 0;
                } else {
                    last_tap = now;                 /* first tap; wait for a partner */
                }
            }
        }

        /* Start listening as soon as the press qualifies as a hold, rather
         * than making the user wait until they let go. */
        if (held && !listening && now - pressed_at >= PTT_HOLD_MS) {
            listening = true;
            last_tap  = 0;
            if (ui_screen_is_off()) ui_screen_on();
            audio_sound_async(SND_READY);   /* audible "go ahead", no need to look */
            ui_set_mood(MOOD_LISTENING);
            ui_say("listening...");
            voice_start();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Silence on the wire has to mean "the board stopped", not "the log buffer
 * filled up". The default USB-CDC console blocks the calling task once the
 * host stops draining it, which can wedge whichever task happened to log -
 * including the LVGL task. The driver-backed VFS discards instead of waiting. */
static void console_make_nonblocking(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 2048;
    cfg.rx_buffer_size = 1024;
    if (usb_serial_jtag_driver_install(&cfg) == ESP_OK)
        usb_serial_jtag_vfs_use_driver();
}

/* A fixed pulse with the numbers most likely to explain a hang: if the heap is
 * draining or a specific task stopped feeding the watchdog, this is where it
 * shows up. */
static void heartbeat_task(void *arg)
{
    esp_task_wdt_add(NULL);
    int beat = 0;
    for (;;) {
        /* Feed every second: sleeping the full report interval overshot the
         * watchdog timeout and made this task accuse itself. */
        for (int i = 0; i < 5; i++) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        uint32_t reads, fails, held;
        const char *owner;
        int depth;
        bsp_touch_stats(&reads, &fails);
        bsp_lvgl_lock_info(&owner, &held, &depth);
        ESP_LOGI(TAG, "hb %d | heap %u int | touch %u | lvgl-lock %s d%d %ums",
                 beat++,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)reads, owner, depth, (unsigned)held);
    }
}

/* The UI loop advancing its input counter is the one signal that covers every
 * layer at once: FreeRTOS scheduling, the LVGL task, the touch bus and the
 * mutex. If it stops while the rest of the system is plainly alive, something
 * is wedged in a way no amount of guessing has found - so record what is
 * holding the lock and restart, which turns a dead board into a blink.
 *
 * This is a backstop, not a fix. Every trip is a bug worth reading the log for. */
#define LIVENESS_STALL_MS 20000

static void liveness_task(void *arg)
{
    uint32_t last_reads = 0, stalled_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        uint32_t reads, fails;
        bsp_touch_stats(&reads, &fails);
        if (reads != last_reads) {
            last_reads = reads;
            stalled_ms = 0;
            continue;
        }

        stalled_ms += 1000;
        if (stalled_ms < LIVENESS_STALL_MS) continue;

        const char *owner;
        uint32_t held;
        int depth;
        bsp_lvgl_lock_info(&owner, &held, &depth);
        ESP_LOGE(TAG, "UI stalled %u ms; lvgl-lock owner=%s depth=%d held=%ums; "
                      "heap %u int / %u psram",
                 (unsigned)stalled_ms, owner, depth, (unsigned)held,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(200));      /* let the log drain */
        esp_restart();
    }
}

static void batt_task(void *arg)
{
    batt_init();
    for (;;) {
        ui_set_batt(batt_percent());
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

static void clock_task(void *arg)
{
    for (;;) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char buf[16];
        if (net_time_valid()) strftime(buf, sizeof(buf), "%H:%M", &tm);
        else                  strlcpy(buf, "--:--", sizeof(buf));
        ui_set_time(buf);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void net_task(void *arg)
{
    char ssid[33] = "", pass[65] = "";

    ui_boot_step(1, "network stack");

    if (net_load_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ui_boot_step(2, ssid);
        net_connect(ssid, pass);
        if (net_wait_ip(25000)) {
            char ip[20];
            net_get_ip(ip, sizeof(ip));
            ui_set_wifi_state(true);
            ESP_LOGI(TAG, "ip %s", ip);

            net_sntp_start();                 /* lands in the background */
            ui_boot_step(3, "clock");
            xTaskCreate(clock_task, "clock", 3072, NULL, 2, NULL);

            ui_boot_step(4, "proxy");
            voice_connect();                 /* now that there is a network */
            vTaskDelay(pdMS_TO_TICKS(1200));

            ui_boot_done();
            audio_chirp();                     /* proves the output path */
            vTaskDelay(pdMS_TO_TICKS(1500));
            ui_set_mood(MOOD_IDLE);
            vTaskDelete(NULL);
        }
        ui_log("wifi failed - open settings to pick a network");
    } else {
        /* Nothing configured yet: put the picker in front of them rather than
         * making them hunt for the gear. */
        ui_log("no wifi set");
        vTaskDelay(pdMS_TO_TICKS(600));
        ui_settings_open_wifi();
    }

    ui_set_wifi_state(false);
    ui_set_mood(MOOD_OFFLINE);
    vTaskDelete(NULL);
}

void app_main(void)
{
    /* Erasing NVS throws away the wifi credentials, the device identity and
     * every setting, so it is a last resort and it says so loudly when it
     * happens - silently wiping them looked exactly like "the board forgot
     * everything when I unplugged it". */
    console_make_nonblocking();

    /* Printed first thing so an unexplained restart leaves a trace: a panic,
     * a brownout and a bootloader giving up on the image all look identical
     * from the outside otherwise. */
    {
        esp_reset_reason_t r = esp_reset_reason();
        static const char *names[] = {
            "unknown", "power-on", "external", "software", "panic",
            "int-watchdog", "task-watchdog", "other-watchdog",
            "deep-sleep", "brownout", "sdio", "usb", "jtag",
        };
        ESP_LOGW(TAG, "reset reason: %s (%d)",
                 r < sizeof(names)/sizeof(names[0]) ? names[r] : "?", (int)r);
    }

    /* On battery, VBAT only reaches VSYS while the power button is physically
     * held. Latching SYS_EN is what lets go of the button without the board
     * powering down, so it happens before anything else. */
    expander_set(EXIO_SYS_EN, true);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS unusable (%s) - ERASING credentials and settings",
                 esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init: %s", esp_err_to_name(err));
    }

    nvs_stats_t st;
    if (nvs_get_stats(NULL, &st) == ESP_OK)
        ESP_LOGI(TAG, "NVS %d/%d entries used, %d free, %d namespaces",
                 st.used_entries, st.total_entries, st.free_entries, st.namespace_count);
    settings_load();

    /* Backlight starts dark so the panel's first garbage frame is never seen. */
    bsp_backlight_init(0);
    ESP_ERROR_CHECK(bsp_display_start());
    ui_init();
    ui_boot_step(0, "display");
    for (int d = 0; d <= settings_get()->brightness; d += 8) {
        bsp_backlight_set(d);
        vTaskDelay(pdMS_TO_TICKS(12));
    }
    bsp_backlight_set(settings_get()->brightness);

    /* Wifi grabs its buffers before the codec claims what is left of internal
     * RAM, and neither failure is fatal - a board that boots without sound or
     * without network is still debuggable, a boot loop is not. */
    esp_err_t nerr = net_init();
    if (nerr != ESP_OK) {
        ESP_LOGE(TAG, "net_init failed: %s", esp_err_to_name(nerr));
        ui_log("network init failed");
    }
    if (audio_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed");
        ui_log("audio init failed");
    }
    /* Starts the one connection the board keeps: audio up, audio and text
     * down, and anything the proxy sends unprompted. */
    /* After audio: the IMU shares the system I2C bus that the codec stands up. */
    if (imu_start() != ESP_OK) ESP_LOGW(TAG, "no IMU - the face will not lean");

    if (voice_init() == ESP_OK) voice_start_capture_task();
    else ESP_LOGE(TAG, "voice client failed to start");
    if (nerr == ESP_OK) xTaskCreate(net_task, "net", 5120, NULL, 4, NULL);
    xTaskCreate(ptt_task, "ptt", 3072, NULL, 3, NULL);
    xTaskCreate(batt_task, "batt", 3072, NULL, 2, NULL);
    xTaskCreate(heartbeat_task, "hb", 3584, NULL, 1, NULL);
    xTaskCreate(pwr_task, "pwr", 3072, NULL, 3, NULL);
    xTaskCreate(liveness_task, "liveness", 3584, NULL, 6, NULL);

    probe_start();

    ESP_LOGI(TAG, "free heap %lu, psram %u",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
