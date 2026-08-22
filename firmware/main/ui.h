#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Pet moods, driven by what the node is actually doing. */
typedef enum {
    MOOD_BOOT,
    MOOD_IDLE,
    MOOD_LISTENING,
    MOOD_THINKING,
    MOOD_SPEAKING,
    MOOD_HAPPY,
    MOOD_DIZZY,      /* just been shaken       */
    MOOD_DOZING,     /* idle too long, napping */
    MOOD_OFFLINE,
} ui_mood_t;

void ui_init(void);
void ui_set_mood(ui_mood_t mood);
void ui_say(const char *text);        /* transient overlay, auto-hides */
/* The two halves of a turn. What was heard sits dim above what came back, so
 * a misheard question is obvious without waiting for the answer to make no
 * sense. */
void ui_set_heard(const char *text);
void ui_set_reply(const char *text);
void ui_log(const char *fmt, ...);

/* top strip: time | wifi | battery | gear */
void ui_set_time(const char *text);
void ui_set_wifi_state(bool connected);
void ui_set_batt(int percent);        /* -1 = no pack, running on USB */
void ui_set_gw_ok(bool ok);

/* Any user/gateway activity: resets the nap timer and wakes him up. */
/* Live mic level, 0..100. Drives the speaker grille while listening so the
 * robot visibly reacts to your voice. */
void ui_set_level(int percent);

/* Explicit screen off, as opposed to nodding off on its own. A touch does not
 * undo it - the power button put it away, the power button brings it back. */
void ui_screen_off(void);
void ui_screen_on(void);
bool ui_screen_is_off(void);

/* True for the whole of a turn, including the gaps between spoken sentences.
 * The IMU uses this to keep incidental handling from interrupting one. */
bool ui_is_busy(void);

/* An address arrived after the face had settled into MOOD_OFFLINE. */
void ui_clear_offline(void);

/* Signals that the board is up and ready - once, whoever gets there first. */
void ui_announce_ready(void);

void ui_notice_activity(void);

/* --- what the IMU feeds the face --- */
/* Lean of the board from upright, in degrees. Stored, not drawn: the animation
 * timer picks it up, so a 20 Hz sensor cannot drive a 20 Hz full-frame redraw. */
void ui_set_tilt(int deg);
/* Shaken. Goes cross-eyed and squawks for a couple of seconds, then returns to
 * whatever it was doing. */
void ui_shake(void);
/* Set down screen-first, or picked back up. Like the screen being switched
 * off, except it undoes itself the moment the board moves again. */
void ui_face_down(bool down);

/* Boot sequence. The five grille slats double as a progress bar and the eye
 * shutters wind open one notch per step, so a stalled boot is visible rather
 * than just a blank panel. index runs 0..UI_BOOT_STEPS-1. */
#define UI_BOOT_STEPS 5
void ui_boot_step(int index, const char *what);
void ui_boot_done(void);

/* Re-read font size / nap timeout from settings. */
void ui_apply_settings(void);

/* The face screen, captured once at init. Navigation should always come back
 * here rather than trusting whatever screen happened to be active. */
struct _lv_obj_t;
struct _lv_obj_t *ui_home_screen(void);

#ifdef __cplusplus
}
#endif
