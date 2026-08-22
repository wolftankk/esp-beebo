/*
 * Screen layout for the 172x640 portrait panel.
 *
 *   0 ..  24   status hairline (time / wifi / battery / gear), deliberately dim
 *  24 .. 640   the robot
 *
 * The panel is 1:3.7, which a single face leaves mostly dead. The height is
 * used by three floating bands rather than by drawing a body: an outlined head
 * and torso were tried and read as a costume, so there is no enclosure here at
 * all - just the features, then the band, then the words.
 *
 *   face-local y
 *      74 .. 206   brows, eyes, mouth
 *     280 .. 390   the band: nine bars, or the clock in their place
 *     446 .. 596   subtitle, only while there is something to carry
 *
 * The band is nine bars and a clock sharing one space, because the four things
 * worth showing there - a level, a progress, a waveform, the time - are the
 * same nine rectangles at different heights. One mechanism, four states, and
 * nothing to redraw that is not moving.
 *
 * It stays reductive: no gradients, no shadows (a blurred one cost a watchdog
 * reset once), no outlines. At rest nothing is coloured at all, so the first
 * hint of colour genuinely means something is happening.
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "ui.h"
#include "ui_settings.h"
#include "settings.h"
#include "board_bsp.h"
#include "audio.h"
#include "net.h"
#include "esp_log.h"
#include "fonts.h"

static const char *TAG = "ui";

/* --- palette: one neutral, one accent that changes with state --- */
#define COL_BG      lv_color_hex(0x0a0a0c)
#define COL_FACE    lv_color_hex(0xe8e6e1)
#define COL_FACE_LO lv_color_hex(0x3a3a3e)
#define COL_SUB     lv_color_hex(0xb9b7b2)
#define COL_DIM     lv_color_hex(0x4a4a52)

/* --- geometry --- */
#define BAR_H       24

/* The head is an invisible group, not a drawn one: it exists so the lean can
 * move brows, eyes and mouth together with a single align. */
#define HEAD_W     BSP_LCD_H_RES
#define HEAD_H     190
#define HEAD_Y      40
#define BROW_Y      34          /* head-local */
#define BROW_W      46
#define EYE_W       46
#define EYE_GAP     40
#define EYE_CY      76          /* head-local */
#define EYE_OPEN    46
#define MOUTH_CY   142          /* head-local */
#define EYE_DX     (EYE_GAP / 2 + EYE_W / 2)

#define PANEL_W    114
#define PANEL_H    130
#define PANEL_Y    270          /* face-local */
#define PANEL_CY   (PANEL_H / 2)

#define BARS         9
#define BAR_W       10
#define BAR_GAP      3
#define BAR_MIN      4
#define BAR_MAX    110

static lv_obj_t *s_head, *s_panel;
static lv_obj_t *s_eye[2], *s_brow[2], *s_mouth, *s_smile;
static lv_obj_t *s_bar[BARS], *s_clock;
static lv_obj_t *s_sub, *s_heard, *s_sub_box, *s_time, *s_wifi_ico, *s_batt_ico;
static lv_timer_t *s_sub_hide, *s_dizzy_timer;
static lv_obj_t *s_home_scr;

static lv_point_precise_t s_brow_pts[2][2];

static ui_mood_t s_mood = MOOD_BOOT, s_mood_before_dizzy = MOOD_IDLE;
static bool s_wifi_up, s_gw_up, s_touching;
static bool s_screen_off;      /* switched off deliberately, not a nap */
static bool s_face_down;       /* set down screen-first: quiet until lifted */
static int  s_boot_step, s_level;
static int  s_tilt_target, s_tilt_applied;
static int  s_hist[BARS];
static char s_clock_text[8] = "--:--";
static uint32_t s_last_activity;

#define BL_DOZING 0

/* eye height and nudge, mouth size, brow angle, and the one accent colour on
 * screen - or 0 for "no colour at all", which is what rest looks like */
typedef struct {
    int eye_w, eye_h, eye_dy, gaze_dx, gaze_dy;
    int m_w, m_h;
    int brow_in, brow_out;      /* vertical offset of each brow end */
    bool brows, smile;
    uint32_t accent;
} look_t;

static look_t look_for(ui_mood_t m)
{
    switch (m) {
    /*                       eW  eH  dy  gx  gy   mW mH  bIn bOut brow smile accent */
    case MOOD_LISTENING: return (look_t){ EYE_W, 34,  0,  0,  0, 16,16,  -3,  -5, true, false, 0x4ade80 };
    /* Eyes stay open and glance up - half-closed read as asleep, not busy.
     * The work itself is shown on the chest, where there is room for it. */
    case MOOD_THINKING:  return (look_t){ EYE_W, 26, -2,  0, -5, 20, 5,   4,  -4, true, false, 0xfbbf24 };
    case MOOD_SPEAKING:  return (look_t){ EYE_W, 32,  0,  0,  0, 40,10,  -2,  -2, true, false, 0x60a5fa };
    case MOOD_HAPPY:     return (look_t){ EYE_W, 10, -4,  0,  0,  0, 0,  -6,  -8, true, true,  0xf5f5f4 };
    /* Dizzy: small round eyes that orbit, brows up, mouth a startled dot. */
    case MOOD_DIZZY:     return (look_t){    16, 16,  0,  0,  0, 16,16,  -8,  -6, true, false, 0xa78bfa };
    case MOOD_DOZING:    return (look_t){ EYE_W,  4,  8,  0,  0, 18, 4,   2,   4, false,false, 0 };
    case MOOD_OFFLINE:   return (look_t){ EYE_W,  8,  6,  0,  4, 26, 4,   5,  -1, true, false, 0x52525b };
    case MOOD_BOOT: {
        /* Waking up: the eyes open a notch per stage, so a stalled boot is
         * visible and never mistakable for a blink or a nap. */
        int h = 4 + s_boot_step * 8;
        if (h > EYE_OPEN) h = EYE_OPEN;
        return (look_t){ EYE_W, h, 0, 0, 0, 16, 4, 0, 0, false, false, 0 };
    }
    case MOOD_IDLE:
    default:             return (look_t){ EYE_W, 34,  0,  0,  0, 30, 6,   0,   0, false,false, 0 };
    }
}

/* ------------------------------------------------------------------ face -- */

static void eye_apply(int i, look_t k, bool dim)
{
    int sign = i ? 1 : -1;
    int w = k.eye_w, h = k.eye_h < 4 ? 4 : k.eye_h;
    int r = (h < w ? h : w) / 2;
    /* The head cannot rotate, so the tilt is carried by one eye sitting lower
     * than the other. It reads as a lean far better than it has any right to. */
    int lean = s_tilt_applied / 4;
    if (lean >  6) lean =  6;
    if (lean < -6) lean = -6;

    lv_obj_set_size(s_eye[i], w, h);
    lv_obj_set_style_radius(s_eye[i], r, 0);
    lv_obj_align(s_eye[i], LV_ALIGN_TOP_MID,
                 sign * EYE_DX + k.gaze_dx,
                 EYE_CY - h / 2 + k.eye_dy + k.gaze_dy + sign * lean);
    lv_obj_set_style_bg_color(s_eye[i], dim ? COL_FACE_LO : COL_FACE, 0);
}

static void brow_apply(int i, look_t k, bool dim)
{
    if (!k.brows) {
        lv_obj_add_flag(s_brow[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_brow[i], LV_OBJ_FLAG_HIDDEN);

    /* Inner is the end nearer the middle of the face; swapping the two ends is
     * the whole difference between cross and worried. */
    int cx    = HEAD_W / 2 + (i ? EYE_DX : -EYE_DX) + k.gaze_dx;
    int inner = cx + (i ? -BROW_W / 2 : BROW_W / 2);
    int outer = cx + (i ?  BROW_W / 2 : -BROW_W / 2);
    int base  = BROW_Y + k.eye_dy;

    s_brow_pts[i][0].x = inner;
    s_brow_pts[i][0].y = base + k.brow_in;
    s_brow_pts[i][1].x = outer;
    s_brow_pts[i][1].y = base + k.brow_out;
    lv_line_set_points(s_brow[i], s_brow_pts[i], 2);
    lv_obj_set_style_line_color(s_brow[i], dim ? COL_FACE_LO : COL_FACE, 0);
}

static void mouth_apply(look_t k, bool dim, int w_override, int h_override)
{
    if (k.smile) {
        lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_smile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_color(s_smile, dim ? COL_FACE_LO : COL_FACE, LV_PART_MAIN);
        return;
    }
    lv_obj_add_flag(s_smile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);

    int w = w_override > 0 ? w_override : k.m_w;
    int h = h_override > 0 ? h_override : (k.m_h < 4 ? 4 : k.m_h);
    lv_obj_set_size(s_mouth, w, h);
    lv_obj_set_style_radius(s_mouth, (h < w ? h : w) / 2, 0);
    lv_obj_align(s_mouth, LV_ALIGN_TOP_MID, k.gaze_dx / 2, MOUTH_CY - h / 2);
    lv_obj_set_style_bg_color(s_mouth, dim ? COL_FACE_LO : COL_FACE, 0);
}

/* ----------------------------------------------------------- chest panel -- */

static void bars_hide(bool hide)
{
    for (int i = 0; i < BARS; i++) {
        if (hide) lv_obj_add_flag(s_bar[i], LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_remove_flag(s_bar[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* h in 0..100. Bars grow from the middle outwards, which reads as a waveform
 * rather than a shop-front equaliser. */
static void bar_set(int i, int pct, uint32_t colour)
{
    int h = BAR_MIN + pct * (BAR_MAX - BAR_MIN) / 100;
    if (h < BAR_MIN) h = BAR_MIN;
    if (h > BAR_MAX) h = BAR_MAX;
    lv_obj_set_size(s_bar[i], BAR_W, h);
    lv_obj_set_style_radius(s_bar[i], BAR_W / 2, 0);
    lv_obj_align(s_bar[i], LV_ALIGN_TOP_LEFT,
                 i * (BAR_W + BAR_GAP), PANEL_CY - h / 2);
    lv_obj_set_style_bg_color(s_bar[i], lv_color_hex(colour), 0);
}

static void clock_show(bool show, bool dim)
{
    if (show) {
        lv_obj_remove_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_clock, dim ? COL_FACE_LO : COL_SUB, 0);
    } else {
        lv_obj_add_flag(s_clock, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------------------- */

static void face_apply(ui_mood_t m)
{
    look_t k = look_for(m);
    bool dim = (m == MOOD_OFFLINE || m == MOOD_DOZING);

    for (int i = 0; i < 2; i++) { eye_apply(i, k, dim); brow_apply(i, k, dim); }
    mouth_apply(k, dim, 0, 0);

    if (s_sub)
        lv_obj_set_style_text_color(s_sub, k.accent ? lv_color_hex(k.accent) : COL_SUB, 0);

    /* The panel: bars while something is happening, the clock while not. */
    switch (m) {
    case MOOD_IDLE:
    case MOOD_HAPPY:
        bars_hide(true);  clock_show(true, false);  break;
    case MOOD_DOZING:
        bars_hide(true);  clock_show(true, true);   break;
    case MOOD_OFFLINE:
        bars_hide(true);  clock_show(true, true);   break;
    default:
        bars_hide(false); clock_show(false, false);
        memset(s_hist, 0, sizeof(s_hist));
        for (int i = 0; i < BARS; i++) bar_set(i, 0, k.accent ? k.accent : 0x8a8a92);
        break;
    }
}

static void tilt_apply(void)
{
    int x = s_tilt_applied * 2 / 3;
    if (x >  16) x =  16;
    if (x < -16) x = -16;
    lv_obj_align(s_head, LV_ALIGN_TOP_MID, x, HEAD_Y);
}

/* Blink: the eyes close and reopen. Everything else about the character is
 * still, so this carries most of the sense that it is switched on. */
static void blink_restore_cb(lv_timer_t *t) { face_apply(s_mood); }

static void blink_cb(lv_timer_t *t)
{
    if (s_touching || s_mood == MOOD_OFFLINE || s_mood == MOOD_BOOT ||
        s_mood == MOOD_DOZING || s_mood == MOOD_DIZZY) return;
    look_t k = look_for(s_mood);
    k.eye_h = 4;
    for (int i = 0; i < 2; i++) eye_apply(i, k, false);
    lv_timer_t *r = lv_timer_create(blink_restore_cb, 110, NULL);
    lv_timer_set_repeat_count(r, 1);
    lv_timer_set_period(t, 2600 + (lv_tick_get() % 3200));   /* never metronomic */
}

/* One animation timer for everything. It runs at 12 Hz because that is roughly
 * what a panel rendering whole frames can flush, and the idle states decimate
 * themselves down from there so a robot doing nothing costs nothing. */
#define ANIM_MS 80

static void anim_cb(lv_timer_t *t)
{
    static uint32_t n;
    static const int glance[] = { 0, -5, 0, 4, 0, 0, -3, 0 };
    n++;

    look_t k = look_for(s_mood);
    uint16_t doze_sec = settings_get()->doze_sec;

    /* Lean: applied here rather than from the IMU task, so twenty readings a
     * second cannot turn into twenty full-frame redraws a second. */
    if (!s_screen_off && s_mood != MOOD_BOOT && abs(s_tilt_target - s_tilt_applied) >= 3) {
        s_tilt_applied = s_tilt_target;
        tilt_apply();
        if (!s_touching) for (int i = 0; i < 2; i++) eye_apply(i, k, false);
    }

    switch (s_mood) {

    case MOOD_SPEAKING: {
        /* The mouth is driven by the sound actually leaving the codec, and the
         * same number scrolls across the chest as a waveform. Every other tick
         * is 6 Hz, which still reads as speech and leaves the panel - which
         * flushes 220 KB a frame - room to keep up with everything else. */
        if (n % 2) break;
        int lvl = audio_output_level();
        if (!s_touching) mouth_apply(k, false, 22 + lvl * 32 / 100, 6 + lvl * 26 / 100);
        for (int i = 0; i < BARS - 1; i++) s_hist[i] = s_hist[i + 1];
        s_hist[BARS - 1] = lvl;
        for (int i = 0; i < BARS; i++) bar_set(i, s_hist[i], 0x60a5fa);
        break;
    }

    case MOOD_LISTENING: {
        /* One reading, spread across the bars with a little decay outward, so
         * the panel moves as a body rather than nine independent lamps. */
        static int drawn = -1;
        if (abs(s_level - drawn) >= 5) {
            drawn = s_level;
            for (int i = 0; i < BARS; i++) {
                int d = abs(i - BARS / 2);
                bar_set(i, s_level * (100 - d * 16) / 100, 0x4ade80);
            }
            if (!s_touching) mouth_apply(k, false, 14 + s_level * 40 / 100, 0);
        }
        break;
    }

    case MOOD_THINKING: {
        /* A bump travelling back and forth. Ping-pong rather than wrapping, so
         * it never looks like it jumped. */
        if (n % 2) break;
        static const int pos[] = { 0,1,2,3,4,5,6,7,8,7,6,5,4,3,2,1 };
        int p = pos[(n / 2) % 16];
        for (int i = 0; i < BARS; i++) {
            int d = abs(i - p);
            bar_set(i, d > 2 ? 0 : (100 - d * 38), 0xfbbf24);
        }
        break;
    }

    case MOOD_BOOT: {
        int lit = s_boot_step * BARS / UI_BOOT_STEPS;
        for (int i = 0; i < BARS; i++)
            bar_set(i, i < lit ? 62 : 0, 0x8a8a92);
        /* A slow stage and a stuck one look the same unless something breathes. */
        static const int w[] = { 16, 22, 28, 22 };
        mouth_apply(k, false, w[(n / 4) % 4], 0);
        break;
    }

    case MOOD_DIZZY: {
        /* Eyes orbiting: two dots a quarter-turn apart, which is the cheapest
         * thing that reads as "you shook me". */
        if (n % 2) break;
        static const int ox[] = { 0, 5, 7, 5, 0, -5, -7, -5 };
        static const int oy[] = { -7, -5, 0, 5, 7, 5, 0, -5 };
        int a = (n / 2) % 8, b = (n / 2 + 4) % 8;
        look_t d = k;
        d.gaze_dx = ox[a]; d.gaze_dy = oy[a]; eye_apply(0, d, false);
        d.gaze_dx = ox[b]; d.gaze_dy = oy[b]; eye_apply(1, d, false);
        for (int i = 0; i < BARS; i++)
            bar_set(i, ((i + n) % 3) * 30, 0xa78bfa);
        break;
    }

    case MOOD_IDLE:
        if (n % 4) break;                       /* 3 Hz is plenty for a glance */
        if (!s_touching) {
            k.gaze_dx += glance[(n / 4) % 8];
            for (int e = 0; e < 2; e++) eye_apply(e, k, false);
        }
        if (doze_sec && lv_tick_elaps(s_last_activity) > (uint32_t)doze_sec * 1000) {
            s_mood = MOOD_DOZING;
            bsp_backlight_set(BL_DOZING);
            face_apply(MOOD_DOZING);
            audio_amp_enable(false);
            net_set_power_save(true);
            ESP_LOGI(TAG, "napping: backlight off, amp off, wifi power save on");
        }
        break;

    case MOOD_DOZING:
        /* Nothing is lit, so do almost nothing. */
        if (n % 12) break;
        {
            static const int breath[] = { 4, 4, 5, 6, 6, 5, 4, 4, 3, 3 };
            k.eye_h = breath[(n / 12) % 10];
            for (int e = 0; e < 2; e++) eye_apply(e, k, true);
        }
        break;

    default:
        break;
    }
}

/* Touch: the eyes follow your finger. The only ornament in the whole design,
 * and it earns its place - it is what makes the thing feel aware of you. */
static void face_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_touching = false;
        face_apply(s_mood);
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        ui_notice_activity();
        s_touching = true;
    } else if (code != LV_EVENT_PRESSING) {
        return;
    }

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    look_t k = look_for(s_mood == MOOD_DOZING ? MOOD_IDLE : s_mood);
    int dx = (p.x - BSP_LCD_H_RES / 2) / 9;
    int dy = (p.y - (BAR_H + HEAD_Y + EYE_CY)) / 40;
    if (dx >  9) dx =  9;
    if (dx < -9) dx = -9;
    if (dy >  9) dy =  9;
    if (dy < -7) dy = -7;
    k.gaze_dx = dx;
    k.gaze_dy = dy;
    for (int i = 0; i < 2; i++) { eye_apply(i, k, false); brow_apply(i, k, false); }
}

static void gear_cb(lv_event_t *e)     { ui_settings_open(); }
static void wifi_ico_cb(lv_event_t *e) { ui_settings_open_wifi(); }

void ui_apply_settings(void)
{
    /* Nothing to apply: the overlay uses the pixel face, which exists at one
     * size. Kept so callers do not need to know that. */
}

static lv_obj_t *make_capsule(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, COL_FACE, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void ui_init(void)
{
    bsp_lvgl_lock(-1);

    lv_obj_t *scr = lv_screen_active();
    s_home_scr = scr;
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ---- status hairline: present, never competing for attention ---- */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, BSP_LCD_H_RES, BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    s_time = lv_label_create(bar);
    lv_obj_set_style_text_font(s_time, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_time, COL_DIM, 0);
    lv_obj_align(s_time, LV_ALIGN_LEFT_MID, 8, 0);
    lv_label_set_text(s_time, "--:--");

    s_wifi_ico = lv_label_create(bar);
    lv_obj_set_style_text_font(s_wifi_ico, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_wifi_ico, COL_DIM, 0);
    lv_obj_align(s_wifi_ico, LV_ALIGN_RIGHT_MID, -54, 0);
    lv_label_set_text(s_wifi_ico, LV_SYMBOL_WIFI);
    lv_obj_add_flag(s_wifi_ico, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_wifi_ico, 10);
    lv_obj_add_event_cb(s_wifi_ico, wifi_ico_cb, LV_EVENT_CLICKED, NULL);

    s_batt_ico = lv_label_create(bar);
    lv_obj_set_style_text_font(s_batt_ico, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_ico, COL_DIM, 0);
    lv_obj_align(s_batt_ico, LV_ALIGN_RIGHT_MID, -28, 0);
    lv_label_set_text(s_batt_ico, LV_SYMBOL_USB);

    lv_obj_t *gear = lv_button_create(bar);
    lv_obj_set_size(gear, 24, 22);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_opa(gear, LV_OPA_0, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_add_event_cb(gear, gear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *gl = lv_label_create(gear);
    lv_label_set_text(gl, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_color(gl, COL_DIM, 0);
    lv_obj_center(gl);

    /* ---- the character ---- */
    lv_obj_t *face = lv_obj_create(scr);
    lv_obj_remove_style_all(face);
    lv_obj_set_size(face, BSP_LCD_H_RES, BSP_LCD_V_RES - BAR_H);
    lv_obj_align(face, LV_ALIGN_TOP_MID, 0, BAR_H);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(face, face_touch_cb, LV_EVENT_ALL, NULL);

    /* The band floats: no plate behind it, so it reads as a readout rather
     * than as something bolted to a chest. */
    s_panel = lv_obj_create(face);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, PANEL_Y);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < BARS; i++) {
        s_bar[i] = make_capsule(s_panel);
        bar_set(i, 0, 0x8a8a92);
        lv_obj_add_flag(s_bar[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_clock = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_clock, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_clock, COL_SUB, 0);
    lv_label_set_text(s_clock, s_clock_text);
    lv_obj_center(s_clock);

    /* Transparent: it groups the features so the lean is one align, and is
     * never itself drawn. */
    s_head = lv_obj_create(face);
    lv_obj_remove_style_all(s_head);
    lv_obj_set_size(s_head, HEAD_W, HEAD_H);
    lv_obj_align(s_head, LV_ALIGN_TOP_MID, 0, HEAD_Y);
    lv_obj_set_style_pad_all(s_head, 0, 0);
    lv_obj_remove_flag(s_head, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 2; i++) {
        s_brow[i] = lv_line_create(s_head);
        lv_obj_set_pos(s_brow[i], 0, 0);
        lv_obj_set_style_line_width(s_brow[i], 4, 0);
        lv_obj_set_style_line_rounded(s_brow[i], true, 0);
        lv_obj_set_style_line_color(s_brow[i], COL_FACE, 0);
        s_eye[i] = make_capsule(s_head);
    }
    s_mouth = make_capsule(s_head);

    /* The smile is an arc rather than a bent capsule, and it is the only mood
     * that gets one - which is what makes it land. */
    s_smile = lv_arc_create(s_head);
    lv_obj_remove_style_all(s_smile);
    lv_obj_set_size(s_smile, 64, 64);
    lv_obj_align(s_smile, LV_ALIGN_TOP_MID, 0, MOUTH_CY - 44);
    lv_arc_set_bg_angles(s_smile, 30, 150);
    lv_obj_set_style_arc_color(s_smile, COL_FACE, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_smile, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_smile, true, LV_PART_MAIN);
    lv_obj_remove_flag(s_smile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_smile, LV_OBJ_FLAG_HIDDEN);

    /* ---- subtitle: only present when it has something to carry ---- */
    s_sub_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sub_box);
    lv_obj_set_size(s_sub_box, BSP_LCD_H_RES, 150);
    lv_obj_align(s_sub_box, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_remove_flag(s_sub_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sub_box, LV_OBJ_FLAG_HIDDEN);

    /* Everything on this overlay is set in the pixel face: it is the only one
     * with Chinese glyphs, and a bitmap face at 12 px suits a screen that is
     * otherwise all hard edges. */
    s_heard = lv_label_create(s_sub_box);
    lv_obj_set_width(s_heard, BSP_LCD_H_RES - 24);
    lv_obj_align(s_heard, LV_ALIGN_TOP_MID, 0, 6);
    lv_label_set_long_mode(s_heard, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_heard, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_heard, COL_DIM, 0);
    lv_obj_set_style_text_font(s_heard, &lv_font_pixel_12, 0);
    lv_label_set_text(s_heard, "");

    s_sub = lv_label_create(s_sub_box);
    lv_obj_set_width(s_sub, BSP_LCD_H_RES - 24);
    lv_obj_align(s_sub, LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_sub, COL_SUB, 0);
    lv_obj_set_style_text_font(s_sub, &lv_font_pixel_12, 0);
    lv_label_set_text(s_sub, "");

    s_last_activity = lv_tick_get();
    face_apply(MOOD_BOOT);
    lv_timer_create(blink_cb, 2600, NULL);
    lv_timer_create(anim_cb, ANIM_MS, NULL);

    bsp_lvgl_unlock();
}

lv_obj_t *ui_home_screen(void) { return s_home_scr; }

void ui_boot_step(int index, const char *what)
{
    if (index < 0) index = 0;
    if (index >= UI_BOOT_STEPS) index = UI_BOOT_STEPS - 1;
    if (!bsp_lvgl_lock(1000)) return;
    s_boot_step = index + 1;
    s_mood = MOOD_BOOT;
    face_apply(MOOD_BOOT);
    bsp_lvgl_unlock();
    if (what) ui_say(what);
    ESP_LOGI(TAG, "boot %d/%d: %s", index + 1, UI_BOOT_STEPS, what ? what : "");
}

void ui_boot_done(void)
{
    s_boot_step = UI_BOOT_STEPS;
    ui_set_mood(MOOD_HAPPY);
    ui_say("ready");
}

void ui_set_level(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    /* Attack fast, release slow: a meter that snaps to zero between syllables
     * reads as broken rather than responsive. */
    s_level = (percent > s_level) ? percent : (s_level * 3 + percent) / 4;
}

/* Lean, in degrees, straight from the IMU task. Stored only: applying it here
 * would repaint the whole panel twenty times a second for a head that moves a
 * couple of pixels. The animation timer picks it up. */
void ui_set_tilt(int deg) { s_tilt_target = deg; }

static void dizzy_done_cb(lv_timer_t *t)
{
    s_dizzy_timer = NULL;
    if (s_mood == MOOD_DIZZY) ui_set_mood(s_mood_before_dizzy);
}

void ui_shake(void)
{
    if (s_screen_off || s_face_down) return;
    /* Squawking over the answer it is in the middle of giving is worse than
     * not reacting: picking the board up to look at it is not a complaint. */
    if (audio_is_playing()) return;
    if (s_mood != MOOD_DIZZY) s_mood_before_dizzy =
        (s_mood == MOOD_DOZING) ? MOOD_IDLE : s_mood;
    audio_sound_async(SND_ERROR);
    ui_set_mood(MOOD_DIZZY);
    if (s_dizzy_timer) lv_timer_del(s_dizzy_timer);
    if (bsp_lvgl_lock(500)) {
        s_dizzy_timer = lv_timer_create(dizzy_done_cb, 2200, NULL);
        lv_timer_set_repeat_count(s_dizzy_timer, 1);
        bsp_lvgl_unlock();
    }
}

/* Set down screen-first. Distinct from the power button because it undoes
 * itself the moment you pick the thing back up. */
void ui_face_down(bool down)
{
    if (s_face_down == down) return;
    s_face_down = down;
    if (down) {
        if (s_screen_off) return;
        bsp_backlight_set(0);
        audio_amp_enable(false);
        net_set_power_save(true);
        if (bsp_lvgl_lock(500)) {
            s_mood = MOOD_DOZING;
            face_apply(MOOD_DOZING);
            bsp_lvgl_unlock();
        }
    } else {
        if (s_screen_off) return;
        bsp_backlight_set(settings_get()->brightness);
        net_set_power_save(false);
        s_last_activity = lv_tick_get();
        if (bsp_lvgl_lock(500)) {
            s_mood = MOOD_IDLE;
            face_apply(MOOD_IDLE);
            bsp_lvgl_unlock();
        }
    }
}

void ui_screen_off(void)
{
    if (s_screen_off) return;
    s_screen_off = true;
    bsp_backlight_set(0);
    audio_amp_enable(false);
    net_set_power_save(true);
    if (bsp_lvgl_lock(500)) {
        s_mood = MOOD_DOZING;
        face_apply(MOOD_DOZING);
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "screen off");
}

void ui_screen_on(void)
{
    if (!s_screen_off) return;
    s_screen_off = false;
    bsp_backlight_set(settings_get()->brightness);
    net_set_power_save(false);
    s_last_activity = lv_tick_get();
    if (bsp_lvgl_lock(500)) {
        s_mood = MOOD_IDLE;
        face_apply(MOOD_IDLE);
        bsp_lvgl_unlock();
    }
    ESP_LOGI(TAG, "screen on");
}

bool ui_screen_is_off(void) { return s_screen_off; }

/* Mid-turn: listening, thinking, or partway through an answer. Distinct from
 * "audio is playing", which goes false in the gap between two sentences - long
 * enough for a shake to slip through and squawk over the reply. */
/* The network came back after the face had already given up on it. Only
 * touches the offline state, so it cannot interrupt a turn in progress. */
void ui_clear_offline(void)
{
    if (s_mood != MOOD_OFFLINE || s_screen_off) return;
    ui_set_mood(MOOD_IDLE);
}

bool ui_is_busy(void)
{
    return s_mood == MOOD_LISTENING || s_mood == MOOD_THINKING || s_mood == MOOD_SPEAKING;
}

void ui_notice_activity(void)
{
    if (s_screen_off || s_face_down) return;   /* a stray touch must not undo it */
    s_last_activity = lv_tick_get();
    if (s_mood == MOOD_DOZING) {
        bsp_backlight_set(settings_get()->brightness);
        net_set_power_save(false);
        if (bsp_lvgl_lock(500)) {
            s_mood = MOOD_IDLE;
            face_apply(MOOD_IDLE);
            bsp_lvgl_unlock();
        }
    }
}

void ui_set_mood(ui_mood_t mood)
{
    if (mood != MOOD_IDLE && mood != MOOD_DOZING) s_last_activity = lv_tick_get();
    if (mood == MOOD_IDLE && s_mood == MOOD_DOZING) return;      /* let it sleep */
    if (s_mood == MOOD_DIZZY && mood == MOOD_IDLE && s_dizzy_timer) return;
    if (s_mood == MOOD_DOZING && mood != MOOD_DOZING) {
        bsp_backlight_set(settings_get()->brightness);
        net_set_power_save(false);
    }
    if (!bsp_lvgl_lock(500)) return;
    s_mood = mood;
    face_apply(mood);
    bsp_lvgl_unlock();
}

static void sub_hide_cb(lv_timer_t *t)
{
    lv_obj_add_flag(s_sub_box, LV_OBJ_FLAG_HIDDEN);
    s_sub_hide = NULL;
}

static void show_overlay(void)
{
    lv_obj_remove_flag(s_sub_box, LV_OBJ_FLAG_HIDDEN);
    if (s_sub_hide) lv_timer_del(s_sub_hide);
    s_sub_hide = lv_timer_create(sub_hide_cb, 12000, NULL);
    lv_timer_set_repeat_count(s_sub_hide, 1);
}

void ui_set_heard(const char *text)
{
    if (!s_heard || !bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_heard, text);
    lv_label_set_text(s_sub, "");        /* the answer has not arrived yet */
    show_overlay();
    bsp_lvgl_unlock();
}

void ui_set_reply(const char *text)
{
    if (!s_sub || !bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_sub, text);
    show_overlay();
    bsp_lvgl_unlock();
}

void ui_say(const char *text)
{
    if (!s_sub || !bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_heard, "");
    lv_label_set_text(s_sub, text);
    show_overlay();
    bsp_lvgl_unlock();
}

/* The status bar keeps a small clock because it survives into the settings
 * screens; the chest is where you actually read it. */
void ui_set_time(const char *text)
{
    if (!s_time || !bsp_lvgl_lock(500)) return;
    lv_label_set_text(s_time, text);
    strncpy(s_clock_text, text, sizeof(s_clock_text) - 1);
    if (s_clock) lv_label_set_text(s_clock, s_clock_text);
    bsp_lvgl_unlock();
}

/* dim = no network, neutral = wifi only, accent = gateway reachable too */
static void refresh_wifi_ico(void)
{
    uint32_t c = !s_wifi_up ? 0x4a4a52 : (s_gw_up ? 0x4ade80 : 0x8a8a92);
    if (s_wifi_ico) lv_obj_set_style_text_color(s_wifi_ico, lv_color_hex(c), 0);
}

void ui_set_wifi_state(bool connected)
{
    if (!bsp_lvgl_lock(500)) return;
    s_wifi_up = connected;
    refresh_wifi_ico();
    bsp_lvgl_unlock();
}

void ui_set_gw_ok(bool ok)
{
    if (!bsp_lvgl_lock(500)) return;
    s_gw_up = ok;
    refresh_wifi_ico();
    bsp_lvgl_unlock();
}

static uint32_t batt_color(int pct)
{
    if (pct < 0) return 0x4a4a52;
    if (pct > 100) pct = 100;
    int r, g, b;
    if (pct >= 50) {                              /* green -> amber */
        int t = (100 - pct) * 255 / 50;
        r = 0x4a + (0xfb - 0x4a) * t / 255;
        g = 0xde + (0xbf - 0xde) * t / 255;
        b = 0x80 + (0x24 - 0x80) * t / 255;
    } else {                                      /* amber -> red   */
        int t = (50 - pct) * 255 / 50;
        r = 0xfb + (0xef - 0xfb) * t / 255;
        g = 0xbf + (0x44 - 0xbf) * t / 255;
        b = 0x24 + (0x44 - 0x24) * t / 255;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

void ui_set_batt(int percent)
{
    if (!s_batt_ico || !bsp_lvgl_lock(500)) return;
    const char *sym;
    if (percent < 0)        sym = LV_SYMBOL_USB;
    else if (percent > 80)  sym = LV_SYMBOL_BATTERY_FULL;
    else if (percent > 55)  sym = LV_SYMBOL_BATTERY_3;
    else if (percent > 30)  sym = LV_SYMBOL_BATTERY_2;
    else if (percent > 12)  sym = LV_SYMBOL_BATTERY_1;
    else                    sym = LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text(s_batt_ico, sym);
    lv_obj_set_style_text_color(s_batt_ico, lv_color_hex(batt_color(percent)), 0);
    bsp_lvgl_unlock();
}

void ui_log(const char *fmt, ...)
{
    char buf[192];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOGI(TAG, "%s", buf);
    ui_say(buf);
}
