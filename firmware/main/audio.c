#include <string.h>
#include <math.h>
#include "audio.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "board_bsp.h"
#include "codec_board.h"
#include "expander.h"
#include "codec_init.h"

static const char *TAG = "audio";

static esp_codec_dev_handle_t s_playback;
static esp_codec_dev_handle_t s_record;
static bool s_rec_open;
static uint8_t *s_rec_raw;

/* Capture and playback are separate codec handles sharing one I2S port, and
 * both the capture task and the playback path close the record side. Closing
 * it twice takes the shared channel down underneath whichever one was still
 * using it, which showed up as a write failing halfway through a reply.
 * Recursive because opening for playback closes the recorder on the way. */
static SemaphoreHandle_t s_codec_mux;

/* What is actually leaving the speaker right now, 0..100. Sampled per DMA
 * chunk on the way out - post-gain, post-limiter - so the mouth follows the
 * sound the room hears rather than the sound the file contains. */
static volatile int s_out_level;


static inline void codec_lock(void)
{
    if (s_codec_mux) xSemaphoreTakeRecursive(s_codec_mux, portMAX_DELAY);
}

static bool s_open;
static int  s_rate, s_ch, s_bits;
static int  s_vol = 70;

/* The ES8311 DAC register already sits near 0 dB at volume 100 and the
 * NS4150B's gain is fixed in hardware, so the only headroom left is digital.
 *
 * Rather than a fixed multiplier, each clip is scaled by its own peak: speech
 * from the TTS service typically lands well below full scale, and a blanket
 * boost would clip the loud clips while leaving the quiet ones quiet. This
 * takes whatever headroom a clip actually has, capped so noise in a nearly
 * silent buffer does not get amplified into a roar.
 *
 * s_boost_x10 rides on top for anyone who wants it hotter still; saturating
 * rather than wrapping, because wrapped overflow sounds like tearing while a
 * clipped peak just sounds loud. */
/* Aiming past full scale on purpose: the peaks are caught by the soft limiter
 * below rather than by hard clipping, which is what lets the average level -
 * the part you actually hear as loudness - rise well beyond what a linear
 * gain allows. Measured TTS clips peak around 15000 of 32767, so a purely
 * linear stage runs out at about 2x. */
#define NORM_TARGET   46000
#define NORM_MAX_X10  60              /* hard ceiling: 6x            */
static int s_boost_x10 = 10;

/* Smooth saturation with a knee: everything below the knee passes untouched,
 * everything above is compressed into the remaining headroom and approaches
 * full scale asymptotically. Speech tolerates this far better than clipping. */
#define LIMIT_KNEE 20000
static inline int16_t scale_sample(int16_t v, int gain_x10)
{
    int32_t x = ((int32_t)v * gain_x10) / 10;
    int32_t a = x < 0 ? -x : x;
    if (a > LIMIT_KNEE) {
        const int32_t head = 32767 - LIMIT_KNEE;
        int32_t over = a - LIMIT_KNEE;
        a = LIMIT_KNEE + (over * head) / (over + head);
        if (a > 32767) a = 32767;
        x = (x < 0) ? -a : a;
    }
    return (int16_t)x;
}

static int normalisation_gain(const int16_t *pcm, size_t samples)
{
    int32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t a = pcm[i] < 0 ? -(int32_t)pcm[i] : pcm[i];
        if (a > peak) peak = a;
    }
    if (peak < 200) return 10;                        /* essentially silence */
    int gain = (int)((int64_t)NORM_TARGET * 10 / peak);
    if (gain < 10) gain = 10;                         /* never attenuate     */
    if (gain > NORM_MAX_X10) gain = NORM_MAX_X10;
    gain = gain * s_boost_x10 / 10;
    if (gain > NORM_MAX_X10) gain = NORM_MAX_X10;
    ESP_LOGI(TAG, "peak %ld -> gain %d.%dx", (long)peak, gain / 10, gain % 10);
    return gain;
}

static esp_err_t ensure_open(int rate, int ch, int bits);

/* Walk the system I2C bus before the codec driver claims it. Expected on this
 * board: TCA9554 @0x20, ES7210 @0x40/0x41, PCF85063 @0x51, QMI8658 @0x6B and
 * ES8311 @0x18. A second ES8311 hung off the same pins would answer at the
 * same 0x18 as the onboard one, and two slaves acking together produce exactly
 * the kind of intermittent, un-debuggable behaviour worth ruling out first. */
static void i2c_scan(void)
{
    i2c_master_bus_config_t cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_NUM_0,
        .scl_io_num                   = BSP_PIN_SYS_SCL,
        .sda_io_num                   = BSP_PIN_SYS_SDA,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "i2c scan: bus busy, skipping");
        return;
    }
    char found[128] = "";
    int n = 0;
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            char one[8];
            snprintf(one, sizeof(one), "0x%02x ", a);
            strlcat(found, one, sizeof(found));
            n++;
        }
    }
    i2c_del_master_bus(bus);
    ESP_LOGW(TAG, "I2C scan (SDA%d/SCL%d): %d device(s): %s",
             BSP_PIN_SYS_SDA, BSP_PIN_SYS_SCL, n, n ? found : "(none)");
}

esp_err_t audio_init(void)
{
    s_codec_mux = xSemaphoreCreateRecursiveMutex();
    if (!s_codec_mux) return ESP_ERR_NO_MEM;

    i2c_scan();

    set_codec_board_type("S3_LCD_3_49");
    codec_init_cfg_t cfg = {
        .in_mode   = CODEC_I2S_MODE_TDM,   /* ES7210 is a 4-channel ADC */
        .out_mode  = CODEC_I2S_MODE_TDM,
        .in_use_tdm = false,
        .reuse_dev  = false,
    };
    esp_err_t err = init_codec(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init_codec: %s", esp_err_to_name(err));
        return err;
    }
    s_playback = get_playback_handle();
    s_record   = get_record_handle();
    if (!s_playback) return ESP_FAIL;

    s_vol = settings_get()->volume;
    /* Deliberately not opening the stream here. Holding it open from boot
     * while nothing plays left the output silent; the codec is opened per
     * burst instead, which is the sequence that actually produced sound. */
    /* The NS4150B's CTRL pin is pulled low by R57, so the amplifier comes up
     * shut down. Nothing else in the stack touches it - enabling it is the
     * difference between "every layer reports success" and audible sound.
     * It is switched off again whenever nothing is playing. */
    audio_amp_enable(true);

    ESP_LOGI(TAG, "codec up (ES8311 out / ES7210 in), volume %d", s_vol);
    return ESP_OK;
}

static bool s_amp_on;
/* Non-zero while a clip is being written to the codec. Muting the amplifier
 * during one cuts the sentence off mid-word, and three separate things ask for
 * that mute - nodding off, the screen going out, the board being set face
 * down - none of which know or should know that audio is in flight. */
static volatile int  s_playing;
static volatile bool s_amp_off_pending;

static inline void codec_unlock(void)
{
    if (s_codec_mux) xSemaphoreGiveRecursive(s_codec_mux);
}

void audio_amp_enable(bool on)
{
    if (!on && s_playing) {
        /* Deferred, not ignored: whatever wanted silence still gets it, once
         * the sentence has finished rather than halfway through it. */
        if (!s_amp_off_pending) ESP_LOGI(TAG, "amplifier off deferred - still playing");
        s_amp_off_pending = true;
        return;
    }
    if (on) s_amp_off_pending = false;
    if (on == s_amp_on) return;
    if (expander_set(EXIO_NS_MODE, on) == ESP_OK) {
        s_amp_on = on;
        ESP_LOGI(TAG, "amplifier %s", on ? "on" : "off");
    }
}

bool audio_is_playing(void) { return s_playing != 0; }

/* Called when a clip finishes: honours a mute that arrived while it ran. */
static void playback_ended(void)
{
    if (--s_playing > 0) return;
    s_out_level = 0;
    if (s_amp_off_pending) {
        s_amp_off_pending = false;
        audio_amp_enable(false);
    }
}

void audio_set_volume(int percent)
{
    if (percent < 0)   percent = 0;
    if (percent > 100) percent = 100;
    s_vol = percent;
    if (!s_playback || !s_open) return;      /* re-applied on the next open */
    int rc = esp_codec_dev_set_out_vol(s_playback, (float)percent);
    if (rc != ESP_CODEC_DEV_OK) ESP_LOGW(TAG, "set volume %d: %d", percent, rc);
}

/* Reopening the codec for every clip clicks audibly, so the stream is only
 * torn down when the format actually changes. */
static esp_err_t ensure_open(int rate, int ch, int bits)
{
    audio_amp_enable(true);
    if (s_rec_open) audio_record_stop();      /* one format on the port at a time */
    if (s_open && rate == s_rate && ch == s_ch && bits == s_bits) return ESP_OK;
    if (s_open) {
        esp_codec_dev_close(s_playback);
        s_open = false;
    }
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = rate,
        .channel         = ch,
        .bits_per_sample = bits,
    };
    /* Vendor's working example sets the volume on the closed device, then
     * opens. Doing it on both sides costs nothing and removes the ordering
     * as a variable. */
    esp_codec_dev_set_out_vol(s_playback, (float)s_vol);
    esp_err_t err = esp_codec_dev_open(s_playback, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec open %dHz/%dch/%dbit: %d", rate, ch, bits, (int)err);
        return ESP_FAIL;
    }
    s_open = true; s_rate = rate; s_ch = ch; s_bits = bits;

    /* Volume set while the device was closed does not survive the open, so it
     * gets re-applied every time rather than only at startup. */
    int rc = esp_codec_dev_set_out_vol(s_playback, (float)s_vol);
    ESP_LOGI(TAG, "opened %dHz/%dch/%dbit, volume %d (rc=%d)", rate, ch, bits, s_vol, rc);
    return ESP_OK;
}

esp_err_t audio_play_pcm(const uint8_t *pcm, size_t len,
                         int sample_rate, int channels, int bits)
{
    if (!s_playback || !pcm || !len) return ESP_ERR_INVALID_ARG;
    /* Held across the whole clip: nothing may reopen the port underneath it. */
    codec_lock();
    s_playing++;
    esp_err_t out = ESP_OK;

    /* The codec runs its slots in stereo; feed mono by duplicating samples
     * rather than reconfiguring, which keeps clip-to-clip switching silent. */
    if (channels == 1 && bits == 16) {
        if (ensure_open(sample_rate, 2, 16) != ESP_OK) {
            playback_ended();
            codec_unlock();
            return ESP_FAIL;
        }
        enum { FRAMES = 512 };
        int16_t *stereo = heap_caps_malloc(FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_DMA);
        if (!stereo) {
            ESP_LOGE(TAG, "no DMA-capable memory for the mixdown buffer");
            playback_ended();
            codec_unlock();
            return ESP_ERR_NO_MEM;
        }

        const int16_t *src = (const int16_t *)pcm;
        size_t total = len / 2;
        int gain = normalisation_gain(src, total);
        for (size_t i = 0; i < total; i += FRAMES) {
            size_t n = (total - i < FRAMES) ? total - i : FRAMES;
            for (size_t j = 0; j < n; j++) {
                int16_t v = scale_sample(src[i + j], gain);
                stereo[j * 2]     = v;
                stereo[j * 2 + 1] = v;
            }
            /* Mean absolute amplitude, not peak: speech peaks are spiky and a
             * mouth driven by them snaps open and shut. Attack is immediate,
             * release is damped, which is what reads as a mouth moving. */
            uint32_t sum = 0;
            for (size_t j = 0; j < n; j++) {
                int16_t v = stereo[j * 2];
                sum += (v < 0) ? -(int32_t)v : v;
            }
            int lvl = (int)(sum / (n ? n : 1)) * 100 / 9000;
            if (lvl > 100) lvl = 100;
            s_out_level = (lvl > s_out_level) ? lvl : (s_out_level * 2 + lvl) / 3;

            int rc = esp_codec_dev_write(s_playback, stereo, n * 2 * sizeof(int16_t));
            if (rc != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "codec write failed: %d (wrote %u of %u frames)",
                         rc, (unsigned)i, (unsigned)total);
                free(stereo);
                playback_ended();
                codec_unlock();
                return ESP_FAIL;
            }
        }
        free(stereo);
        ESP_LOGD(TAG, "played %u mono frames @%dHz", (unsigned)total, sample_rate);
        playback_ended();                /* clip over: let the mouth close */
        codec_unlock();
        return ESP_OK;
    }

    if (ensure_open(sample_rate, channels, bits) != ESP_OK) {
        playback_ended();
        codec_unlock();
        return ESP_FAIL;
    }
    int rc = esp_codec_dev_write(s_playback, (void *)pcm, len);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "codec write failed: %d", rc);
        out = ESP_FAIL;
    }
    playback_ended();
    codec_unlock();
    return out;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | (p[1]<<8) | (p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1]<<8); }

esp_err_t audio_play_wav(const uint8_t *wav, size_t len)
{
    if (!wav || len < 44) return ESP_ERR_INVALID_ARG;
    if (memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) {
        ESP_LOGE(TAG, "not a RIFF/WAVE buffer");
        return ESP_ERR_INVALID_ARG;
    }

    int rate = 0, ch = 0, bits = 0;
    size_t pos = 12;
    const uint8_t *data = NULL;
    size_t data_len = 0;

    while (pos + 8 <= len) {
        const uint8_t *id = wav + pos;
        uint32_t sz = rd32(wav + pos + 4);
        size_t body = pos + 8;

        if (!memcmp(id, "fmt ", 4) && body + 16 <= len) {
            ch   = rd16(wav + body + 2);
            rate = (int)rd32(wav + body + 4);
            bits = rd16(wav + body + 14);
        } else if (!memcmp(id, "data", 4)) {
            /* Trust the buffer, not the header: the TTS service emits
             * streaming placeholders here (~0x7FFFFFFF). */
            data     = wav + body;
            data_len = len - body;
            if (sz != 0 && sz < data_len) data_len = sz;
            break;
        }
        if (sz == 0 || sz > len) break;           /* malformed or placeholder */
        pos = body + sz + (sz & 1);
    }

    if (!data || !rate || !bits) {
        ESP_LOGE(TAG, "no usable fmt/data chunk");
        return ESP_ERR_INVALID_ARG;
    }
    ESP_LOGI(TAG, "wav %dHz %dch %dbit, %u bytes of audio",
             rate, ch, bits, (unsigned)data_len);
    return audio_play_pcm(data, data_len, rate, ch ? ch : 1, bits);
}

/* The array runs 32-bit slots in TDM; the vendor's own capture path uses
 * 24 kHz / 2 channels / 32 bits, and setting the input gain is not optional -
 * it defaults to nothing and the buffers come back silent. */
#define REC_RATE   24000
#define REC_CH     2
#define REC_BITS   32
#define REC_GAIN   30.0f
#define REC_CHUNK  1024

esp_err_t audio_record_start(void)
{
    if (!s_record) return ESP_ERR_INVALID_STATE;
    codec_lock();
    if (s_rec_open) { codec_unlock(); return ESP_OK; }

    if (s_open) {                              /* free the port */
        esp_codec_dev_close(s_playback);
        s_open = false;
    }
    if (!s_rec_raw) {
        s_rec_raw = heap_caps_malloc(REC_CHUNK, MALLOC_CAP_DMA);
        if (!s_rec_raw) { codec_unlock(); return ESP_ERR_NO_MEM; }
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = REC_RATE,
        .channel         = REC_CH,
        .bits_per_sample = REC_BITS,
    };
    esp_codec_dev_set_in_gain(s_record, REC_GAIN);
    int rc = esp_codec_dev_open(s_record, &fs);
    if (rc != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "record open failed: %d", rc);
        codec_unlock();
        return ESP_FAIL;
    }
    esp_codec_dev_set_in_gain(s_record, REC_GAIN);   /* re-apply after open */
    s_rec_open = true;
    ESP_LOGI(TAG, "recording %dHz/%dch/%dbit, gain %.0f", REC_RATE, REC_CH, REC_BITS, REC_GAIN);
    codec_unlock();
    return ESP_OK;
}

void audio_record_stop(void)
{
    codec_lock();
    if (s_rec_open) {
        esp_codec_dev_close(s_record);
        s_rec_open = false;
    }
    codec_unlock();
}

int audio_record_read(int16_t *out, int max_samples)
{
    if (!out || max_samples <= 0) return -1;
    codec_lock();
    if (!s_rec_open) { codec_unlock(); return -1; }

    int frame_bytes = REC_CH * (REC_BITS / 8);           /* 8 */
    int want = max_samples * frame_bytes;
    if (want > REC_CHUNK) want = REC_CHUNK;

    int rrc = esp_codec_dev_read(s_record, s_rec_raw, want);
    codec_unlock();
    if (rrc != ESP_CODEC_DEV_OK) return -1;

    /* Keep the first channel and take the top 16 bits of each 32-bit slot. */
    const int32_t *src = (const int32_t *)s_rec_raw;
    int frames = want / frame_bytes;
    for (int i = 0; i < frames; i++) out[i] = (int16_t)(src[i * REC_CH] >> 16);
    return frames;
}

/* Astromech vocabulary: swept sine with vibrato, which is how those beeps are
 * actually made. Phase accumulates across the whole sequence so frequency can
 * jump between segments without clicking - only amplitude discontinuities
 * click, and the envelope dips at each boundary to articulate the chirps.
 */
typedef struct {
    int   f0, f1;      /* glide start and end, Hz           */
    int   ms;
    float vib_hz;      /* vibrato rate, 0 for a clean sweep */
    float vib_depth;
} chirp_seg_t;

static const chirp_seg_t k_boot[] = {
    {  620, 1500, 110,  0.0f, 0.00f },
    { 1700,  900,  80,  0.0f, 0.00f },
    { 1050, 1050, 130, 22.0f, 0.06f },
    {  760, 1900, 120,  0.0f, 0.00f },
    { 2000, 1450,  90, 14.0f, 0.04f },
};
/* Short enough not to delay speaking - it fires the moment the key is held. */
static const chirp_seg_t k_ready[] = {
    { 1100, 1500, 55, 0.0f, 0.0f },
    { 1800, 2000, 55, 0.0f, 0.0f },
};
static const chirp_seg_t k_ok[] = {
    {  900, 1400, 70, 0.0f, 0.0f },
};
static const chirp_seg_t k_error[] = {
    { 1300,  700, 140, 26.0f, 0.10f },
    {  600,  420, 190, 18.0f, 0.08f },
};
static const chirp_seg_t k_sleep[] = {
    { 1400,  600, 260, 10.0f, 0.05f },
    {  600,  180, 340,  6.0f, 0.07f },
};

static void play_chirps(const chirp_seg_t *segs, size_t n, const char *name)
{
    enum { RATE = 24000 };
    int total_ms = 0;
    for (size_t i = 0; i < n; i++) total_ms += segs[i].ms;
    int total = RATE * total_ms / 1000;

    int16_t *buf = heap_caps_malloc((size_t)total * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "%s: out of memory", name);
        return;
    }

    float phase = 0.0f;
    int at = 0;
    for (size_t seg = 0; seg < n; seg++) {
        const chirp_seg_t *c = &segs[seg];
        int len = RATE * c->ms / 1000;
        for (int i = 0; i < len && at < total; i++, at++) {
            float t = (float)i / len;
            float f = c->f0 + (c->f1 - c->f0) * t;
            if (c->vib_hz > 0.0f)
                f *= 1.0f + c->vib_depth * sinf(2.0f * (float)M_PI * c->vib_hz * i / RATE);

            phase += 2.0f * (float)M_PI * f / RATE;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;

            float env = 1.0f, edge = 0.12f;
            if (t < edge)          env = t / edge;
            else if (t > 1 - edge) env = (1 - t) / edge;

            buf[at] = (int16_t)(9000.0f * env * sinf(phase));
        }
    }
    ESP_LOGI(TAG, "%s: %d ms", name, total_ms);
    audio_play_pcm((const uint8_t *)buf, (size_t)at * sizeof(int16_t), RATE, 1, 16);
    free(buf);
}

void audio_sound(audio_sound_t which)
{
    switch (which) {
    case SND_READY: play_chirps(k_ready, sizeof(k_ready)/sizeof(k_ready[0]), "ready"); break;
    case SND_OK:    play_chirps(k_ok,    sizeof(k_ok)/sizeof(k_ok[0]),       "ok");    break;
    case SND_ERROR: play_chirps(k_error, sizeof(k_error)/sizeof(k_error[0]), "error"); break;
    case SND_SLEEP: play_chirps(k_sleep, sizeof(k_sleep)/sizeof(k_sleep[0]), "sleep"); break;
    case SND_BOOT:
    default:        play_chirps(k_boot,  sizeof(k_boot)/sizeof(k_boot[0]),   "boot");  break;
    }
}

void audio_chirp(void) { audio_sound(SND_BOOT); }

static void sound_task(void *arg)
{
    audio_sound((audio_sound_t)(intptr_t)arg);
    vTaskDelete(NULL);
}

/* Playing inline from a UI callback would freeze the interface. */
void audio_sound_async(audio_sound_t which)
{
    xTaskCreatePinnedToCore(sound_task, "snd", 4096, (void *)(intptr_t)which, 4, NULL, 1);
}

void audio_chime_async(void) { audio_sound_async(SND_BOOT); }

int audio_output_level(void) { return s_out_level; }
