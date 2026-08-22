/*
 * Settings screens for a 172 px wide panel.
 *
 * The width matters more than it looks: 172 px across a 3.49" 172x640 panel is
 * about 23 mm of glass. A stock 10-column LVGL keyboard would give 2.3 mm keys,
 * which no finger can hit. So the password keyboard here is 4 columns x 8 rows
 * with ~43x42 px (roughly 5.7x5.6 mm) keys, paged between abc / ABC / 123.
 * Slower to type, but it actually works.
 *
 * Scanning and connecting run on their own tasks - doing either on the LVGL
 * task would freeze rendering and eventually trip the idle watchdog.
 */
#include <string.h>
#include "ui_settings.h"
#include "ui.h"
#include "net.h"
#include "voice.h"
#include "settings.h"
#include "batt.h"
#include "audio.h"
#include "board_bsp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "ui_set";

#define W   BSP_LCD_H_RES
#define PAD 8

static lv_obj_t *s_menu, *s_wifi;
static lv_obj_t *s_ap_list, *s_wifi_status, *s_wifi_row;
static lv_obj_t *s_pw_scr, *s_pw_ta, *s_kb, *s_pw_status;
static lv_obj_t *s_font_preview;
static volatile bool s_scanning;   /* one scan at a time */
static lv_obj_t *s_wifi_info;
static lv_obj_t *s_wifi_return;   /* menu or home, depending on entry point */
static char s_sel_ssid[33];
static bool s_reusing_pass;   /* connecting with a stored key, not a typed one */

/* ---------------- helpers ---------------- */

static lv_obj_t *make_screen(const char *title, lv_event_cb_t back_cb)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d0b09), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, W, 40);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1c1815), 0);

    lv_obj_t *back = lv_button_create(bar);
    lv_obj_set_size(back, 40, 34);
    lv_obj_align(back, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(back, lv_color_hex(0x3a332b), 0);
    lv_obj_set_ext_click_area(back, 14);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);

    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, lv_color_hex(0xffab3d), 0);
    lv_obj_align(t, LV_ALIGN_RIGHT_MID, -10, 0);
    return scr;
}

static lv_obj_t *make_body(lv_obj_t *scr)
{
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, W, BSP_LCD_V_RES - 40);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(body, PAD, 0);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);   /* never claim horizontal drags */
    return body;
}

/* A button matrix only enters the checked state if its buttons are marked
 * CHECKABLE first. Setting CHECKED alone leaves the highlight stuck on the
 * initial choice, so a tap saved the value but looked like it had failed. */
static void make_segmented(lv_obj_t *m, int count, int selected)
{
    for (int i = 0; i < count; i++)
        lv_buttonmatrix_set_button_ctrl(m, i, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(m, true);
    lv_buttonmatrix_set_button_ctrl(m, selected, LV_BUTTONMATRIX_CTRL_CHECKED);

    lv_obj_set_style_bg_color(m, lv_color_hex(0xffab3d),
                              LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(m, lv_color_hex(0x1a1410),
                                LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x33302a), LV_PART_ITEMS);
    lv_obj_set_style_text_color(m, lv_color_hex(0xc2b596), LV_PART_ITEMS);
}

static lv_obj_t *section_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0x8a8070), 0);
    return l;
}

/* ---------------- wifi page ---------------- */

static void wifi_back_cb(lv_event_t *e)
{
    lv_obj_t *dest = s_wifi_return ? s_wifi_return : (lv_obj_t *)ui_home_screen();
    lv_screen_load(dest);
    lv_obj_del_async(s_wifi);
    /* Every pointer into this page dies with it. Leaving even one behind lets
     * the scan task, which finishes seconds later, walk freed LVGL objects -
     * that spun forever inside lv_obj_clean() while holding the LVGL lock. */
    s_wifi        = NULL;
    s_wifi_info   = NULL;
    s_wifi_status = NULL;
    s_ap_list     = NULL;
    s_wifi_row    = NULL;
}

/* Current-link card, shown above the scan list whenever we are associated. */
static void wifi_info_refresh(void)
{
    if (!s_wifi_info) return;
    char ssid[33] = "", ip[20] = "";
    net_get_ssid(ssid, sizeof(ssid));
    net_get_ip(ip, sizeof(ip));
    int rssi = net_get_rssi();
    int pct  = batt_percent();

    if (!ssid[0]) {
        lv_label_set_text(s_wifi_info, "not connected");
        return;
    }
    const char *bars = rssi > -55 ? "excellent" : rssi > -67 ? "good"
                     : rssi > -75 ? "fair" : "weak";
    if (pct >= 0)
        lv_label_set_text_fmt(s_wifi_info, "%s\nip   %s\nsig  %d dBm (%s)\nbatt %d%% (%.2f V)",
                              ssid, ip, rssi, bars, pct, batt_volts());
    else
        lv_label_set_text_fmt(s_wifi_info, "%s\nip   %s\nsig  %d dBm (%s)\npower USB",
                              ssid, ip, rssi, bars);
}

static void forget_cb(lv_event_t *e)
{
    net_clear_creds();
    lv_label_set_text(s_wifi_info, "credentials cleared");
}

static void connect_task(void *arg)
{
    char pass[65];
    strlcpy(pass, (const char *)arg, sizeof(pass));
    free(arg);

    net_connect(s_sel_ssid, pass);
    bool ok = net_wait_ip(20000);

    if (bsp_lvgl_lock(1000)) {
        if (s_wifi) wifi_info_refresh();
        if (s_pw_scr && s_pw_status) {
            lv_label_set_text(s_pw_status, ok ? "connected" : "wrong password?");
            lv_obj_set_style_text_color(s_pw_status,
                lv_color_hex(ok ? 0x7ee06b : 0xe0705b), 0);
        }
        bsp_lvgl_unlock();
    }
    if (ok) {
        net_save_creds(s_sel_ssid, pass);
    } else if (s_reusing_pass) {
        /* The remembered key is stale. Drop it so the picker stops silently
         * failing and asks for a new one. */
        net_forget(s_sel_ssid);
        if (bsp_lvgl_lock(1000)) {
            if (s_wifi_info)
                lv_label_set_text(s_wifi_info, "saved password no longer works\ntap the network again");
            bsp_lvgl_unlock();
        }
    }
    s_reusing_pass = false;
    vTaskDelete(NULL);
}

/* 4 columns so the keys are big enough to hit on a 23 mm wide panel. */
static const char *kb_lower[] = {
    "a","b","c","d","\n", "e","f","g","h","\n", "i","j","k","l","\n",
    "m","n","o","p","\n", "q","r","s","t","\n", "u","v","w","x","\n",
    "y","z",".","-","\n",
    LV_SYMBOL_BACKSPACE,"123","ABC",LV_SYMBOL_OK,"" };
static const char *kb_upper[] = {
    "A","B","C","D","\n", "E","F","G","H","\n", "I","J","K","L","\n",
    "M","N","O","P","\n", "Q","R","S","T","\n", "U","V","W","X","\n",
    "Y","Z","_","+","\n",
    LV_SYMBOL_BACKSPACE,"123","abc",LV_SYMBOL_OK,"" };
static const char *kb_num[] = {
    "1","2","3","4","\n", "5","6","7","8","\n", "9","0","_","-","\n",
    "!","@","#","$","\n", "%","^","&","*","\n", "(",")","+","=","\n",
    ".",",",":",";","\n",
    LV_SYMBOL_BACKSPACE,"abc","ABC",LV_SYMBOL_OK,"" };

static void kb_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    const char *k = lv_buttonmatrix_get_button_text(kb, lv_buttonmatrix_get_selected_button(kb));
    if (!k) return;

    if (!strcmp(k, LV_SYMBOL_BACKSPACE))      lv_textarea_delete_char(s_pw_ta);
    else if (!strcmp(k, "123"))               lv_buttonmatrix_set_map(kb, kb_num);
    else if (!strcmp(k, "ABC"))               lv_buttonmatrix_set_map(kb, kb_upper);
    else if (!strcmp(k, "abc"))               lv_buttonmatrix_set_map(kb, kb_lower);
    else if (!strcmp(k, LV_SYMBOL_OK)) {
        char *pw = malloc(65);
        strlcpy(pw, lv_textarea_get_text(s_pw_ta), 65);
        lv_label_set_text(s_pw_status, "connecting...");
        lv_obj_set_style_text_color(s_pw_status, lv_color_hex(0xffab3d), 0);
        xTaskCreatePinnedToCore(connect_task, "wificonn", 4096, pw, 4, NULL, 0);
    } else {
        lv_textarea_add_text(s_pw_ta, k);
    }
}

/* ---- proxy address ----
 * Same keyboard, different destination. Opens on the numeric page because an
 * address is mostly digits, a dot and a colon. */
static lv_obj_t *s_px_scr, *s_px_ta, *s_px_status;

static void px_back_cb(lv_event_t *e)
{
    lv_screen_load(s_menu ? s_menu : (lv_obj_t *)ui_home_screen());
    lv_obj_del_async(s_px_scr);
    s_px_scr = NULL;
    s_px_ta = s_px_status = NULL;
}

static void px_kb_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    const char *k = lv_buttonmatrix_get_button_text(kb, lv_buttonmatrix_get_selected_button(kb));
    if (!k) return;

    if (!strcmp(k, LV_SYMBOL_BACKSPACE))      lv_textarea_delete_char(s_px_ta);
    else if (!strcmp(k, "123"))               lv_buttonmatrix_set_map(kb, kb_num);
    else if (!strcmp(k, "ABC"))               lv_buttonmatrix_set_map(kb, kb_upper);
    else if (!strcmp(k, "abc"))               lv_buttonmatrix_set_map(kb, kb_lower);
    else if (!strcmp(k, LV_SYMBOL_OK)) {
        net_set_proxy_host(lv_textarea_get_text(s_px_ta));
        char url[128];
        net_get_proxy_url(url, sizeof(url));
        lv_label_set_text_fmt(s_px_status, "%s\nreconnecting...", url);
        lv_obj_set_style_text_color(s_px_status, lv_color_hex(0xffab3d), 0);
        voice_reconnect();
    } else {
        lv_textarea_add_text(s_px_ta, k);
    }
}

static void px_open_cb(lv_event_t *e)
{
    s_px_scr = make_screen("PROXY", px_back_cb);

    lv_obj_t *hint = lv_label_create(s_px_scr);
    lv_obj_set_width(hint, W - 12);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8a8070), 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 42);
    lv_label_set_text(hint, "address of the machine\nrunning beebo-proxy");

    s_px_ta = lv_textarea_create(s_px_scr);
    lv_obj_set_size(s_px_ta, W - 12, 46);
    lv_obj_align(s_px_ta, LV_ALIGN_TOP_MID, 0, 78);
    lv_textarea_set_one_line(s_px_ta, true);
    lv_textarea_set_placeholder_text(s_px_ta, "192.168.1.42");
    lv_obj_set_style_text_font(s_px_ta, &lv_font_montserrat_18, 0);
    char host[96];
    net_get_proxy_host(host, sizeof(host));
    if (host[0]) lv_textarea_set_text(s_px_ta, host);

    s_px_status = lv_label_create(s_px_scr);
    lv_obj_set_width(s_px_status, W - 12);
    lv_label_set_long_mode(s_px_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_px_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_px_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_px_status, lv_color_hex(0x8a8070), 0);
    lv_obj_align(s_px_status, LV_ALIGN_TOP_MID, 0, 128);
    char url[128];
    net_get_proxy_url(url, sizeof(url));
    lv_label_set_text(s_px_status, url);

    lv_obj_t *kb = lv_buttonmatrix_create(s_px_scr);
    lv_buttonmatrix_set_map(kb, kb_num);
    lv_obj_set_size(kb, W - 8, 340);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(kb, px_kb_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_screen_load(s_px_scr);
}

static void pw_back_cb(lv_event_t *e)
{
    lv_screen_load(s_wifi ? s_wifi : (lv_obj_t *)ui_home_screen());
    lv_obj_del_async(s_pw_scr);
    s_pw_scr = NULL;
    s_pw_ta = s_kb = s_pw_status = NULL;
}

/* The password entry gets its own screen with fixed coordinates. Nesting the
 * keyboard inside the scrolling flex list pushed its lower rows out of the
 * visible area, so the drawn keys and the ones you could actually hit stopped
 * lining up. Absolute positioning removes the whole class of problem, and the
 * full-height layout buys 42x63 px keys (about 5.6 x 8.4 mm). */
static void ap_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *ssid = lv_list_get_button_text(s_ap_list, btn);
    if (!ssid) return;
    strlcpy(s_sel_ssid, ssid, sizeof(s_sel_ssid));

    /* A network the board has been on before does not need its password typed
     * in again on a 23 mm keyboard. If the stored one turns out to be wrong -
     * the network changed its key - connect_task forgets it, so the next tap
     * asks properly. */
    char *known = malloc(65);
    if (known && net_known_pass(s_sel_ssid, known, 65)) {
        s_reusing_pass = true;
        if (s_wifi_info) lv_label_set_text_fmt(s_wifi_info, "connecting to\n%s ...", s_sel_ssid);
        xTaskCreatePinnedToCore(connect_task, "wificonn", 4096, known, 4, NULL, 0);
        return;
    }
    free(known);
    s_reusing_pass = false;

    s_pw_scr = make_screen("PASSWORD", pw_back_cb);

    lv_obj_t *name = lv_label_create(s_pw_scr);
    lv_obj_set_width(name, W - 12);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(0xc2b596), 0);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 46);
    lv_label_set_text(name, s_sel_ssid);

    s_pw_ta = lv_textarea_create(s_pw_scr);
    lv_obj_set_size(s_pw_ta, W - 12, 46);
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_MID, 0, 68);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "password");
    lv_obj_set_style_text_font(s_pw_ta, &lv_font_montserrat_18, 0);

    s_pw_status = lv_label_create(s_pw_scr);
    lv_obj_set_style_text_font(s_pw_status, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pw_status, lv_color_hex(0x8a8070), 0);
    lv_obj_align(s_pw_status, LV_ALIGN_TOP_MID, 0, 120);
    lv_label_set_text(s_pw_status, "");

    s_kb = lv_buttonmatrix_create(s_pw_scr);
    lv_buttonmatrix_set_map(s_kb, kb_lower);
    lv_obj_set_size(s_kb, W - 4, BSP_LCD_V_RES - 144);
    lv_obj_align(s_kb, LV_ALIGN_TOP_MID, 0, 142);
    lv_obj_set_style_pad_all(s_kb, 2, 0);
    lv_obj_set_style_pad_row(s_kb, 2, 0);
    lv_obj_set_style_pad_column(s_kb, 2, 0);
    lv_obj_set_style_text_font(s_kb, &lv_font_montserrat_18, 0);
    lv_obj_add_event_cb(s_kb, kb_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_screen_load(s_pw_scr);
}

static void scan_task(void *arg)
{
    wifi_ap_record_t *aps = NULL;
    uint16_t n = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = net_scan(&aps, &n);
    int64_t t1 = esp_timer_get_time();
    s_scanning = false;

    if (bsp_lvgl_lock(2000)) {
        /* s_wifi is the authority on whether the page outlived the scan. */
        if (s_wifi && s_ap_list) {
            lv_obj_clean(s_ap_list);
            if (err != ESP_OK || n == 0) {
                lv_list_add_text(s_ap_list, "no networks found");
            } else {
                for (int i = 0; i < n; i++) {
                    /* The S3 has no 5 GHz radio, so anything listed is usable. */
                    const char *icon = aps[i].rssi > -60 ? LV_SYMBOL_WIFI : LV_SYMBOL_MINUS;
                    lv_obj_t *b = lv_list_add_button(s_ap_list, icon, (const char *)aps[i].ssid);
                    lv_obj_add_event_cb(b, ap_click_cb, LV_EVENT_CLICKED, NULL);
                    lv_obj_set_style_text_font(b, &lv_font_montserrat_14, 0);
                }
            }
        }
        if (s_wifi && s_wifi_status) lv_label_set_text_fmt(s_wifi_status, "%u networks", n);
        bsp_lvgl_unlock();
    }
    int64_t t2 = esp_timer_get_time();
    ESP_LOGI(TAG, "scan=%lldms populate=%lldms n=%u err=%d",
             (t1 - t0) / 1000, (t2 - t1) / 1000, n, (int)err);
    vTaskDelete(NULL);
}

static void rescan_cb(lv_event_t *e)
{
    if (s_scanning) return;              /* stacking scans hangs the wifi driver */
    s_scanning = true;
    lv_obj_clean(s_ap_list);
    lv_list_add_text(s_ap_list, "scanning...");
    xTaskCreatePinnedToCore(scan_task, "wifiscan", 4096, NULL, 4, NULL, 0);
}

static void wifi_open_cb(lv_event_t *e)
{
    s_wifi_return = s_menu ? s_menu : ui_home_screen();
    s_wifi = make_screen("WI-FI", wifi_back_cb);
    lv_obj_t *body = make_body(s_wifi);
    lv_obj_set_style_pad_row(body, 6, 0);

    s_wifi_info = lv_label_create(body);
    lv_obj_set_width(s_wifi_info, W - 2 * PAD - 6);
    lv_label_set_long_mode(s_wifi_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_wifi_info, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_info, lv_color_hex(0xc2b596), 0);
    wifi_info_refresh();

    lv_obj_t *fg = lv_button_create(body);
    lv_obj_set_size(fg, W - 2 * PAD - 6, 30);
    lv_obj_set_style_bg_color(fg, lv_color_hex(0x50352e), 0);
    lv_obj_add_event_cb(fg, forget_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *fgl = lv_label_create(fg);
    lv_label_set_text(fgl, "forget network");
    lv_obj_set_style_text_font(fgl, &lv_font_montserrat_14, 0);
    lv_obj_center(fgl);

    s_wifi_status = lv_label_create(body);
    lv_label_set_text(s_wifi_status, "scanning...");
    lv_obj_set_style_text_color(s_wifi_status, lv_color_hex(0xffab3d), 0);

    lv_obj_t *btn = lv_button_create(body);
    lv_obj_set_size(btn, W - 2 * PAD, 34);
    lv_obj_add_event_cb(btn, rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, LV_SYMBOL_REFRESH " rescan");
    lv_obj_center(bl);

    s_ap_list = lv_list_create(body);
    lv_obj_set_size(s_ap_list, W - 2 * PAD, 380);


    lv_screen_load(s_wifi);
    if (!s_scanning) {
        s_scanning = true;
        xTaskCreatePinnedToCore(scan_task, "wifiscan", 4096, NULL, 4, NULL, 0);
    }
}

/* ---------------- main settings menu ---------------- */

static void menu_back_cb(lv_event_t *e)
{
    settings_save();
    settings_apply();
    lv_screen_load((lv_obj_t *)ui_home_screen());
    lv_obj_del_async(s_menu);
    s_menu = NULL;
}

/* Everything here persists the moment it changes. Saving only on the way out
 * meant one broken exit path silently discarded every setting.
 *
 * These are steppers rather than sliders on purpose. A slider sitting inside
 * the scrolling settings column loses its drag to the parent's gesture
 * handling, and dragging a 146 px track on a 23 mm wide panel was never going
 * to be pleasant anyway. Two big buttons and a read-only bar always work. */
typedef struct {
    uint8_t *value;
    int      min, max, step;
    bool     is_brightness;
    lv_obj_t *label, *bar;
    const char *name;
} stepper_t;

static void stepper_refresh(stepper_t *st)
{
    lv_label_set_text_fmt(st->label, "%s   %d", st->name, *st->value);
    lv_bar_set_value(st->bar, *st->value, LV_ANIM_OFF);
}

static void stepper_cb(lv_event_t *e)
{
    stepper_t *st = lv_event_get_user_data(e);
    int delta = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));

    int v = (int)*st->value + delta * st->step;
    if (v < st->min) v = st->min;
    if (v > st->max) v = st->max;
    *st->value = (uint8_t)v;

    if (st->is_brightness) bsp_backlight_set(*st->value);
    else                   audio_set_volume(*st->value);
    stepper_refresh(st);
    settings_save();
    ESP_LOGI(TAG, "%s -> %d", st->name, v);
}

static const lv_font_t *font_for_idx(uint8_t i)
{
    switch (i) {
    case 0:  return &lv_font_montserrat_14;
    case 2:  return &lv_font_montserrat_24;
    default: return &lv_font_montserrat_18;
    }
}

static void test_sound_cb(lv_event_t *e)
{
    audio_chime_async();
}

static void font_cb(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    uint8_t idx = lv_buttonmatrix_get_selected_button(m);
    settings_get()->font = idx;
    /* The size only shows up on the subtitle overlay back on the home screen,
     * which is hidden most of the time - so show it here, immediately. */
    if (s_font_preview) lv_obj_set_style_text_font(s_font_preview, font_for_idx(idx), 0);
    ui_apply_settings();
    settings_save();
    ESP_LOGI(TAG, "font -> %u", idx);
}

static void doze_cb(lv_event_t *e)
{
    static const uint16_t opts[] = { 30, 90, 300, 0 };
    lv_obj_t *m = lv_event_get_target(e);
    settings_get()->doze_sec = opts[lv_buttonmatrix_get_selected_button(m)];
    settings_save();
    ESP_LOGI(TAG, "nap -> %us", settings_get()->doze_sec);
}

/* One entry per stepper on the menu. The pool is rebuilt with the screen -
 * an earlier version kept a never-reset counter here, so the third time you
 * opened settings the slots were exhausted and the controls silently
 * stopped being created. */
#define STEPPER_MAX 2
static stepper_t s_steppers[STEPPER_MAX];
static int       s_stepper_used;

static void add_stepper(lv_obj_t *body, const char *name, uint8_t *value,
                        int min, int max, int step, bool is_brightness)
{
    if (s_stepper_used >= STEPPER_MAX) {
        ESP_LOGE(TAG, "stepper pool exhausted - \"%s\" not created", name);
        return;
    }
    stepper_t *st = &s_steppers[s_stepper_used++];
    st->value = value; st->min = min; st->max = max; st->step = step;
    st->is_brightness = is_brightness; st->name = name;

    st->label = section_label(body, name);

    lv_obj_t *row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, W - 2 * PAD - 6, 40);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    const char *glyph[2] = { LV_SYMBOL_MINUS, LV_SYMBOL_PLUS };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_button_create(row);
        lv_obj_set_size(b, 38, 38);
        lv_obj_align(b, i ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x4a4234), 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)(i ? 1 : -1));
        lv_obj_add_event_cb(b, stepper_cb, LV_EVENT_CLICKED, st);
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, glyph[i]);
        lv_obj_center(l);
    }

    st->bar = lv_bar_create(row);
    lv_obj_set_size(st->bar, W - 2 * PAD - 6 - 88, 10);
    lv_obj_center(st->bar);
    lv_bar_set_range(st->bar, min, max);
    lv_obj_set_style_bg_color(st->bar, lv_color_hex(0xffab3d), LV_PART_INDICATOR);

    stepper_refresh(st);
}

/* Double-tapping BOOT always lands you back on the robot, whatever screen you
 * got lost on. Each teardown nulls its own pointers for the same reason the
 * back handlers do - background tasks outlive these screens. */
void ui_settings_close_all(void)
{
    if (!bsp_lvgl_lock(1000)) return;
    lv_screen_load((lv_obj_t *)ui_home_screen());

    if (s_pw_scr) {
        lv_obj_del_async(s_pw_scr);
        s_pw_scr = NULL;
        s_pw_ta = s_kb = s_pw_status = NULL;
    }
    if (s_wifi) {
        lv_obj_del_async(s_wifi);
        s_wifi = NULL;
        s_wifi_info = s_wifi_status = s_ap_list = s_wifi_row = NULL;
    }
    if (s_menu) {
        lv_obj_del_async(s_menu);
        s_menu = NULL;
    }
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "escaped to home");
}

void ui_settings_open_wifi(void)
{
    if (s_wifi) return;
    ui_notice_activity();
    if (!bsp_lvgl_lock(1000)) return;
    wifi_open_cb(NULL);
    bsp_lvgl_unlock();
}

void ui_settings_open(void)
{
    if (s_menu) return;
    ui_notice_activity();

    if (!bsp_lvgl_lock(1000)) return;
    s_stepper_used = 0;                 /* pool belongs to this screen */
    s_menu = make_screen("SETTINGS", menu_back_cb);
    lv_obj_t *body = make_body(s_menu);
    settings_t *st = settings_get();

    /* --- wifi --- */
    section_label(body, "network");
    s_wifi_row = lv_button_create(body);
    lv_obj_set_size(s_wifi_row, W - 2 * PAD - 10, 40);
    lv_obj_add_event_cb(s_wifi_row, wifi_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *wl = lv_label_create(s_wifi_row);
    char ssid[33] = "", pass[65] = "";
    if (net_load_creds(ssid, sizeof(ssid), pass, sizeof(pass)) && ssid[0])
        lv_label_set_text_fmt(wl, LV_SYMBOL_WIFI "  %s", ssid);
    else
        lv_label_set_text(wl, LV_SYMBOL_WIFI "  not set");
    lv_obj_center(wl);

    lv_obj_t *px_row = lv_button_create(body);
    lv_obj_set_size(px_row, W - 2 * PAD - 10, 40);
    lv_obj_add_event_cb(px_row, px_open_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pxl = lv_label_create(px_row);
    lv_obj_set_style_text_font(pxl, &lv_font_montserrat_14, 0);
    char phost[96];
    net_get_proxy_host(phost, sizeof(phost));
    lv_label_set_text_fmt(pxl, LV_SYMBOL_UPLOAD "  %s", phost[0] ? phost : "proxy: built-in");
    lv_obj_center(pxl);

    add_stepper(body, "brightness", &st->brightness, 10, 255, 25, true);
    add_stepper(body, "volume", &st->volume, 0, 100, 10, false);

    lv_obj_t *ts = lv_button_create(body);
    lv_obj_set_size(ts, W - 2 * PAD - 6, 40);
    lv_obj_set_style_bg_color(ts, lv_color_hex(0x3f5a3a), 0);
    lv_obj_add_event_cb(ts, test_sound_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tsl = lv_label_create(ts);
    lv_label_set_text(tsl, LV_SYMBOL_VOLUME_MAX "  test sound");
    lv_obj_set_style_text_font(tsl, &lv_font_montserrat_14, 0);
    lv_obj_center(tsl);


    /* --- font size --- */
    section_label(body, "font size");
    static const char *font_map[] = { "S", "M", "L", "" };
    lv_obj_t *fm = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(fm, font_map);
    lv_obj_set_size(fm, W - 2 * PAD - 10, 52);
    make_segmented(fm, 3, st->font);
    lv_obj_add_event_cb(fm, font_cb, LV_EVENT_VALUE_CHANGED, NULL);

    s_font_preview = lv_label_create(body);
    lv_obj_set_width(s_font_preview, W - 2 * PAD - 6);
    lv_label_set_long_mode(s_font_preview, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_font_preview, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_font_preview, lv_color_hex(0xc2b596), 0);
    lv_obj_set_style_text_font(s_font_preview, font_for_idx(st->font), 0);
    lv_label_set_text(s_font_preview, "subtitle preview");

    /* --- doze --- */
    section_label(body, "nap after");
    static const char *doze_map[] = { "30s", "90s", "5m", "off", "" };
    lv_obj_t *dm = lv_buttonmatrix_create(body);
    lv_buttonmatrix_set_map(dm, doze_map);
    lv_obj_set_size(dm, W - 2 * PAD - 10, 52);
    int di = st->doze_sec == 30 ? 0 : st->doze_sec == 90 ? 1 : st->doze_sec == 300 ? 2 : 3;
    make_segmented(dm, 4, di);
    lv_obj_add_event_cb(dm, doze_cb, LV_EVENT_VALUE_CHANGED, NULL);

    /* --- about --- */
    section_label(body, "about");
    lv_obj_t *about = lv_label_create(body);
    lv_obj_set_width(about, W - 2 * PAD - 10);
    lv_label_set_long_mode(about, LV_LABEL_LONG_WRAP);
    char ip[20] = "-";
    net_get_ip(ip, sizeof(ip));
    const esp_app_desc_t *app = esp_app_get_description();
    lv_label_set_text_fmt(about, "ip %s\nfw %s\npsram %u KB free",
                          ip, app->version,
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    lv_obj_set_style_text_color(about, lv_color_hex(0x6b6152), 0);

    lv_screen_load(s_menu);
    bsp_lvgl_unlock();
    ESP_LOGI(TAG, "settings opened");
}
