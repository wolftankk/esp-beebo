#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t audio_init(void);
void      audio_set_volume(int percent);
/* The NS4150B draws current and hisses whenever it is enabled, so it is only
 * powered while there is something to play. */
void      audio_amp_enable(bool on);       /* 0..100 */

/* Raw interleaved PCM. mono is upmixed to the codec's stereo slots. */
esp_err_t audio_play_pcm(const uint8_t *pcm, size_t len,
                         int sample_rate, int channels, int bits);

/* Parses a RIFF/WAVE buffer and plays its data chunk.
 * The declared chunk sizes are deliberately ignored - the TTS service writes
 * streaming placeholders (0x7FFFFFxx, i.e. ~2 GB) rather than real lengths,
 * so the buffer length is the only trustworthy bound. */
esp_err_t audio_play_wav(const uint8_t *wav, size_t len);

/* Short two-note chime, used to prove the output path end to end. */
/* Capture from the ES7210 array. Playback and capture share one I2S port in
 * TDM mode and cannot be open with different formats at once, so starting a
 * recording tears the playback stream down and vice versa. */
esp_err_t audio_record_start(void);
void      audio_record_stop(void);
/* Fills `out` with mono 16-bit samples taken from the first mic channel.
 * Returns the sample count, or a negative value on error. */
int       audio_record_read(int16_t *out, int max_samples);

/* Astromech vocabulary. Swept, warbling sine chirps - the character comes
 * from the pitch gliding, not from the notes. Used only where the sound
 * carries information; a beep for every state change would just be noise. */
typedef enum {
    SND_BOOT,     /* rising, curious - powering up         */
    SND_READY,    /* two quick notes - listening now       */
    SND_OK,       /* short affirmative                     */
    SND_ERROR,    /* descending warble - that did not work */
    SND_SLEEP,    /* long glide down, winding away         */
} audio_sound_t;

void      audio_sound(audio_sound_t which);
void      audio_sound_async(audio_sound_t which);
/* Mean output amplitude of the clip currently playing, 0..100, or 0 when
 * nothing is. Drives the mouth: the data is already in hand, so a lip-synced
 * face costs one pass over a buffer that was being copied anyway. */
int       audio_output_level(void);

/* True while a clip is being written out. Asking for the amplifier off during
 * one defers the mute until it finishes rather than clipping the sentence. */
bool      audio_is_playing(void);

void      audio_chirp(void);   /* SND_BOOT, kept for existing call sites */
void      audio_chime_async(void);   /* safe to call from a UI callback */

#ifdef __cplusplus
}
#endif
