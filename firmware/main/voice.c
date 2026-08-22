/*
 * The board's one connection.
 *
 * Audio goes up while you are still speaking rather than after you let go,
 * which on a weak link is most of the wait. Replies come back a sentence at a
 * time and are played as they land, so the robot starts talking before the
 * agent has finished writing. The same socket carries messages the server
 * sends unprompted - the reason this is a socket at all.
 *
 * The board used to hold its own gateway credential and speak OpenClaw's
 * protocol directly. It no longer does: the proxy holds that, and everything
 * here is audio and small JSON.
 */
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "voice.h"
#include "audio.h"
#include "ui.h"
#include "net.h"
#include "rtc.h"
#include <sys/time.h>
#include <stdlib.h>
#include "esp_websocket_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "beebo_config.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "cJSON.h"

static const char *TAG = "voice";

#define REC_RATE       24000
#define FRAME_SAMPLES  512                  /* ~21 ms per frame on the wire */
#define MAX_SEGMENT    (700 * 1024)
/* Deep enough to hold a long answer. Synthesis outruns playback by a wide
 * margin - the proxy sends a sentence every second or so while the speaker
 * needs three to say it - so the queue is where a whole reply accumulates.
 * A sentence of speech is roughly 150 KB of PSRAM, so eight is about 1.2 MB
 * of the 7.5 MB free. */
#define SEGMENT_QUEUE  8

typedef struct { uint8_t *data; int len;
    bool     hold_gain;   /* tail of a split clause: keep the head's gain */
} segment_t;

static esp_websocket_client_handle_t s_ws;
static QueueHandle_t s_segments;
static volatile bool s_online, s_capturing, s_busy;
/* Resolved at connect time rather than baked in, so changing the address in
 * settings takes effect without a reflash. */
static char s_url[128];
/* When a frame - any frame, including the proxy's keepalive ping - last
 * arrived. The proxy pings every 20 s and terminates a client that misses one,
 * so a board whose radio slept through the exchange ends up holding a socket
 * the server has already closed. Nothing in the client notices: it reports
 * connected, writes appear to succeed, and the next thing anyone finds out is
 * that the robot has been deaf for an hour. */
static uint32_t s_last_rx;
#define LIVENESS_MS 70000        /* three missed pings, with room to spare */
static uint32_t s_last_attempt;
static bool     s_seg_hold_gain;

/* Reassembly for the current inbound message. */
static uint8_t *s_seg;
static int      s_seg_len, s_seg_cap;
static char    *s_txt;
static int      s_txt_len, s_txt_cap;
static int      s_cur_op;

static void ws_send_json(const char *json)
{
    if (s_online) esp_websocket_client_send_text(s_ws, json, strlen(json), pdMS_TO_TICKS(2000));
}

/* ---------- inbound ---------- */

static void handle_text(const char *json, int len)
{
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return;
    const cJSON *t = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(t)) { cJSON_Delete(root); return; }
    const char *type = t->valuestring;

    /* Partial text while the agent is still writing. Same label as the final
     * reply, just replaced as it grows, so the screen fills as the words
     * arrive instead of staying blank until the whole answer exists. */
    /* The proxy's clock and zone. Arrives on connect and hourly after that,
     * which means the board is right without anyone configuring anything, and
     * stays right across a daylight-saving change. */
    if (!strcmp(type, "time")) {
        const cJSON *tz = cJSON_GetObjectItem(root, "tz");
        if (cJSON_IsString(tz)) net_set_timezone(tz->valuestring);

        const cJSON *ep = cJSON_GetObjectItem(root, "epoch");
        if (cJSON_IsNumber(ep) && ep->valuedouble > 1735689600.0) {
            struct timeval tv = { .tv_sec = (time_t)ep->valuedouble, .tv_usec = 0 };
            time_t before = time(NULL);
            settimeofday(&tv, NULL);
            /* Only worth a flash write when it actually moved. */
            if (llabs((long long)tv.tv_sec - (long long)before) > 2) {
                ESP_LOGI(TAG, "clock set from the proxy (was off by %llds)",
                         (long long)(tv.tv_sec - before));
                rtc_save_system_time();
            }
        }

    } else if (!strcmp(type, "reply.partial")) {
        const cJSON *x = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(x)) ui_set_reply(x->valuestring);

    } else if (!strcmp(type, "ready")) {
        ESP_LOGI(TAG, "proxy ready");
        ui_set_gw_ok(true);

    } else if (!strcmp(type, "state")) {
        const cJSON *v = cJSON_GetObjectItem(root, "value");
        if (cJSON_IsString(v) && !strcmp(v->valuestring, "thinking")) ui_set_mood(MOOD_THINKING);
        else ui_set_mood(MOOD_IDLE);

    } else if (!strcmp(type, "heard")) {
        const cJSON *x = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(x)) { ESP_LOGI(TAG, "heard: %s", x->valuestring); ui_set_heard(x->valuestring); }

    } else if (!strcmp(type, "reply")) {
        const cJSON *x = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(x)) { ESP_LOGI(TAG, "reply: %s", x->valuestring); ui_set_reply(x->valuestring); }

    } else if (!strcmp(type, "audio.begin")) {
        const cJSON *b = cJSON_GetObjectItem(root, "bytes");
        int want = (cJSON_IsNumber(b) && b->valueint > 0) ? b->valueint : 128 * 1024;
        if (want > MAX_SEGMENT) want = MAX_SEGMENT;
        free(s_seg);
        s_seg = heap_caps_malloc(want, MALLOC_CAP_SPIRAM);
        s_seg_cap = s_seg ? want : 0;
        s_seg_len = 0;
        const cJSON *hg = cJSON_GetObjectItem(root, "hold_gain");
        s_seg_hold_gain = cJSON_IsTrue(hg);

    } else if (!strcmp(type, "audio.end")) {
        if (s_seg && s_seg_len > 44) {
            segment_t seg = { .data = s_seg, .len = s_seg_len, .hold_gain = s_seg_hold_gain };
            /* Waits for room rather than discarding. Dropping on a full queue
             * is what cut long answers off mid-sentence: the player is always
             * behind by design, so "behind" was never a reason to throw the
             * rest of the reply away. Blocking here backs pressure up the
             * socket, which is where it belongs. */
            if (xQueueSend(s_segments, &seg, pdMS_TO_TICKS(20000)) != pdTRUE) {
                ESP_LOGE(TAG, "dropped %d bytes: player still busy after 20 s", s_seg_len);
                free(s_seg);
            }
            s_seg = NULL;
            s_seg_cap = s_seg_len = 0;
        }

    } else if (!strcmp(type, "turn.done")) {
        s_busy = false;

    } else if (!strcmp(type, "notify")) {
        /* Unprompted. Wake the screen: something happened that was not asked for. */
        const cJSON *x = cJSON_GetObjectItem(root, "text");
        if (cJSON_IsString(x)) {
            ESP_LOGI(TAG, "notify: %s", x->valuestring);
            if (ui_screen_is_off()) ui_screen_on();
            ui_notice_activity();
            ui_say(x->valuestring);
        }

    } else if (!strcmp(type, "error")) {
        const cJSON *m = cJSON_GetObjectItem(root, "message");
        ESP_LOGE(TAG, "proxy: %s", cJSON_IsString(m) ? m->valuestring : "?");
        ui_log("%s", cJSON_IsString(m) ? m->valuestring : "error");
        audio_sound_async(SND_ERROR);
        s_busy = false;
        ui_set_mood(MOOD_IDLE);
    }
    cJSON_Delete(root);
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED: {
        s_online = true;
        s_last_rx = xTaskGetTickCount();
        char hello[192];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"device\":\"%s\",\"fw\":\"0.2.0\",\"token\":\"%s\"}",
                 BEEBO_DEVICE_ID, BEEBO_PROXY_TOKEN);
        ws_send_json(hello);
        ESP_LOGI(TAG, "connected to %s", s_url);
        break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED:
        s_online = false;
        s_busy = false;
        ui_set_gw_ok(false);
        break;

    case WEBSOCKET_EVENT_DATA:
        s_last_rx = xTaskGetTickCount();
        if (e->op_code == 0x08) { s_online = false; break; }
        if (e->op_code == 0x09 || e->op_code == 0x0A) break;      /* ping / pong */

        /* Continuation frames carry op_code 0, so remember what started. */
        if (e->payload_offset == 0) s_cur_op = e->op_code;

        if (s_cur_op == 0x02) {                                   /* audio */
            if (s_seg && s_seg_len + e->data_len <= s_seg_cap) {
                memcpy(s_seg + s_seg_len, e->data_ptr, e->data_len);
                s_seg_len += e->data_len;
            } else if (s_seg) {
                ESP_LOGW(TAG, "segment overflow: %d + %d > %d",
                         s_seg_len, (int)e->data_len, s_seg_cap);
            }
        } else if (s_cur_op == 0x01) {                            /* json */
            if (e->payload_offset == 0) s_txt_len = 0;
            if (s_txt_len + e->data_len + 1 > s_txt_cap) {
                int cap = s_txt_len + e->data_len + 256;
                char *p = realloc(s_txt, cap);
                if (!p) break;
                s_txt = p;
                s_txt_cap = cap;
            }
            memcpy(s_txt + s_txt_len, e->data_ptr, e->data_len);
            s_txt_len += e->data_len;
            s_txt[s_txt_len] = '\0';
            if (e->payload_offset + e->data_len >= e->payload_len)
                handle_text(s_txt, s_txt_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "websocket error");
        break;

    default:
        break;
    }
}

/* ---------- playback ---------- */

static void player_task(void *arg)
{
    segment_t seg;
    for (;;) {
        if (xQueueReceive(s_segments, &seg, portMAX_DELAY) != pdTRUE) continue;
        ui_set_mood(MOOD_SPEAKING);
        if (seg.hold_gain) audio_hold_next_gain();
        audio_play_wav(seg.data, seg.len);
        free(seg.data);
        /* Only settle once nothing else is waiting, so consecutive sentences
         * do not flicker the face between them. */
        if (uxQueueMessagesWaiting(s_segments) == 0 && !s_busy) ui_set_mood(MOOD_IDLE);
    }
}

/* ---------- capture ---------- */

static void capture_task(void *arg)
{
    int16_t *frame = heap_caps_malloc(FRAME_SAMPLES * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    if (!frame) vTaskDelete(NULL);
    bool open = false;

    for (;;) {
        if (!s_capturing) {
            if (open) { audio_record_stop(); open = false; ui_set_level(0); }
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        if (!open) {
            if (audio_record_start() != ESP_OK) { vTaskDelay(pdMS_TO_TICKS(250)); continue; }
            open = true;
        }

        int n = audio_record_read(frame, FRAME_SAMPLES);
        if (n <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

        /* Straight out as it is recorded: by the time the key is released the
         * audio is already at the far end. */
        if (s_online)
            esp_websocket_client_send_bin(s_ws, (const char *)frame,
                                          n * sizeof(int16_t), pdMS_TO_TICKS(1000));

        int64_t sum = 0;
        for (int i = 0; i < n; i++) sum += (int32_t)frame[i] * frame[i];
        int pct = (int)sqrtf((float)(sum / n)) / 60;
        ui_set_level(pct > 100 ? 100 : pct);
    }
}

/* ---------- api ---------- */

esp_err_t voice_init(void)
{
    s_segments = xQueueCreate(SEGMENT_QUEUE, sizeof(segment_t));
    if (!s_segments) return ESP_ERR_NO_MEM;
    xTaskCreatePinnedToCore(player_task, "player", 4096, NULL, 4, NULL, 0);
    return ESP_OK;
}

/* Rebuilds the socket when it has been down long enough that the client's own
 * retry is not going to fix it. Two failure modes need this, and neither shows
 * up as an error: a socket left half-open by a nap, where the board believes
 * it is connected until the first write fails minutes later, and a client that
 * exhausted its retries while there was no network at all. */
static void ws_supervisor_task(void *arg)
{
    int down_ticks = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!s_ws) continue;

        if (!net_is_up()) { down_ticks = 0; continue; }   /* not our problem yet */

        if (s_online) {
            down_ticks = 0;
            /* Believing we are connected is not evidence of it. */
            uint32_t quiet = (xTaskGetTickCount() - s_last_rx) * portTICK_PERIOD_MS;
            if (quiet > LIVENESS_MS) {
                ESP_LOGW(TAG, "nothing from the proxy for %lu s - socket is stale",
                         (unsigned long)(quiet / 1000));
                s_online = false;
                ui_set_gw_ok(false);
                esp_websocket_client_stop(s_ws);
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_websocket_client_start(s_ws);
                s_last_rx = xTaskGetTickCount();
            }
            continue;
        }

        /* Give the client's own reconnect a fair chance first. */
        if (++down_ticks < 4) continue;
        down_ticks = 0;

        ESP_LOGW(TAG, "socket down 20 s with a working network - rebuilding");
        esp_websocket_client_stop(s_ws);
        vTaskDelay(pdMS_TO_TICKS(200));
        if (esp_websocket_client_start(s_ws) != ESP_OK)
            ESP_LOGE(TAG, "restart failed");
    }
}

void voice_kick(void)
{
    if (!s_ws || s_online) return;
    /* An attempt already in flight is not stuck. Without this, the got-IP
     * handler and the boot task both fire and the second one tears down a
     * handshake the first had halfway done. */
    uint32_t since = (xTaskGetTickCount() - s_last_attempt) * portTICK_PERIOD_MS;
    if (s_last_attempt && since < 6000) return;
    s_last_attempt = xTaskGetTickCount();
    ESP_LOGI(TAG, "network back - reopening the socket");
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_start(s_ws);
}

void voice_reconnect(void)
{
    if (!s_ws) return;
    net_get_proxy_url(s_url, sizeof(s_url));
    ESP_LOGI(TAG, "reconnecting to %s", s_url);
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_set_uri(s_ws, s_url);
    esp_websocket_client_start(s_ws);
}

/* Idempotent, and safe to call from the got-IP handler. It used to be reached
 * only from the boot task, behind a 25 s wait for an address - so on a network
 * that took longer than that to associate, the socket was never opened at all
 * and the board sat there permanently offline with a perfectly good link. */
esp_err_t voice_connect(void)
{
    if (s_ws) { voice_kick(); return ESP_OK; }

    s_last_attempt = xTaskGetTickCount();
    net_get_proxy_url(s_url, sizeof(s_url));
    esp_websocket_client_config_t cfg = {
        .uri                  = s_url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms   = 10000,
        .buffer_size          = 4096,
        .task_stack           = 6144,
        /* Without these a socket killed by a nap stays "connected" until the
         * next write fails, which is the first thing the user does after
         * waking the board up. Pinging makes the board find out on its own. */
        .keep_alive_enable    = true,
        .keep_alive_idle      = 10,
        .keep_alive_interval   = 5,
        .keep_alive_count     = 3,
        .ping_interval_sec    = 15,
        .pingpong_timeout_sec = 20,
    };
    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) return ESP_FAIL;
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    ESP_ERROR_CHECK(esp_websocket_client_start(s_ws));
    xTaskCreate(ws_supervisor_task, "wssup", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "connecting to %s", s_url);
    return ESP_OK;
}

esp_err_t voice_start_capture_task(void)
{
    return xTaskCreatePinnedToCore(capture_task, "capture", 4096, NULL, 5, NULL, 0) == pdPASS
           ? ESP_OK : ESP_FAIL;
}

void voice_start(void)
{
    if (s_busy || !s_online) {
        if (!s_online) ui_log("proxy offline");
        return;
    }
    net_set_power_save(false);
    ws_send_json("{\"type\":\"turn.begin\"}");
    s_capturing = true;
}

void voice_stop(void)
{
    if (!s_capturing) return;
    s_capturing = false;
    s_busy = true;
    ws_send_json("{\"type\":\"turn.end\"}");
    net_set_power_save(true);
}
