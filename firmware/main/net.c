#include <string.h>
#include <time.h>
#include "net.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "beebo_config.h"
#include "ui.h"
#include "voice.h"
#include "freertos/task.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "net";
static EventGroupHandle_t s_evt;
static esp_netif_t *s_netif;
static bool s_want_connect;
static volatile bool s_link_up;
#define BIT_GOT_IP BIT0
#define BIT_RETRY  BIT1

#define NVS_NS   "beebo"
/* The namespace was "ocn" before the project was renamed. Anything already on
 * a board lives there, and a rename that silently loses somebody's wifi
 * password is not a rename worth doing, so it is read once and carried over. */
#define NVS_NS_LEGACY "ocn"
#define NVS_SSID "ssid"
#define NVS_PASS "pass"
#define NVS_KNOWN "known"
#define NVS_PROXY "proxy"

/* Every network whose password has been typed in, most recent first, so
 * picking a familiar name in the scanner does not ask for it again. One blob
 * rather than a key per network: NVS key names cap at 15 characters and an
 * SSID can be 32. */
#define KNOWN_MAX 6
typedef struct { char ssid[33]; char pass[65]; } known_t;
static known_t s_known[KNOWN_MAX];
static uint8_t s_known_n;

static esp_err_t read_pair(const char *ns, char *ssid, size_t sl, char *pass, size_t pl)
{
    nvs_handle_t h;
    if (nvs_open(ns, NVS_READONLY, &h) != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;
    esp_err_t a = nvs_get_str(h, NVS_SSID, ssid, &sl);
    esp_err_t b = nvs_get_str(h, NVS_PASS, pass, &pl);
    nvs_close(h);
    return (a == ESP_OK && b == ESP_OK && ssid[0]) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}

static void known_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_known);
        if (nvs_get_blob(h, NVS_KNOWN, s_known, &sz) == ESP_OK)
            s_known_n = sz / sizeof(known_t);
        nvs_close(h);
    }
    if (s_known_n) return;

    /* Nothing here yet: pull the single pair the old namespace stored, if any,
     * and seed the list with it. */
    char ssid[33] = "", pass[65] = "";
    const char *from = NULL;
    if (read_pair(NVS_NS, ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK)        from = NVS_NS;
    else if (read_pair(NVS_NS_LEGACY, ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) from = NVS_NS_LEGACY;
    if (!from) return;

    strlcpy(s_known[0].ssid, ssid, sizeof(s_known[0].ssid));
    strlcpy(s_known[0].pass, pass, sizeof(s_known[0].pass));
    s_known_n = 1;
    ESP_LOGI(TAG, "carried \"%s\" over from the \"%s\" namespace", ssid, from);

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_SSID, ssid);
        nvs_set_str(h, NVS_PASS, pass);
        nvs_set_blob(h, NVS_KNOWN, s_known, sizeof(known_t));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void known_remember(const char *ssid, const char *pass)
{
    int at = -1;
    for (int i = 0; i < s_known_n; i++)
        if (!strcmp(s_known[i].ssid, ssid)) { at = i; break; }

    if (at < 0) {
        at = (s_known_n < KNOWN_MAX) ? s_known_n++ : KNOWN_MAX - 1;
    }
    /* Move to the front: the list is ordered by how recently it worked, so
     * the oldest entry is the one that falls off. */
    for (int i = at; i > 0; i--) s_known[i] = s_known[i - 1];
    strlcpy(s_known[0].ssid, ssid, sizeof(s_known[0].ssid));
    strlcpy(s_known[0].pass, pass, sizeof(s_known[0].pass));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KNOWN, s_known, s_known_n * sizeof(known_t));
    nvs_commit(h);
    nvs_close(h);
}

void net_forget(const char *ssid)
{
    int at = -1;
    for (int i = 0; i < s_known_n; i++)
        if (!strcmp(s_known[i].ssid, ssid)) { at = i; break; }
    if (at < 0) return;

    for (int i = at; i + 1 < s_known_n; i++) s_known[i] = s_known[i + 1];
    s_known_n--;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (s_known_n) nvs_set_blob(h, NVS_KNOWN, s_known, s_known_n * sizeof(known_t));
    else           nvs_erase_key(h, NVS_KNOWN);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "forgot \"%s\"", ssid);
}

bool net_known_pass(const char *ssid, char *pass, size_t len)
{
    for (int i = 0; i < s_known_n; i++)
        if (!strcmp(s_known[i].ssid, ssid)) { strlcpy(pass, s_known[i].pass, len); return true; }
    return false;
}

bool net_is_up(void) { return s_link_up; }

/* ---- where the proxy is ----
 *
 * Compiled in is fine for a board you flash yourself, but the machine running
 * the proxy is on DHCP and its address moves. Having to reflash the firmware
 * because a laptop got a new lease is not a reasonable thing to ask, so the
 * address is editable on the device and lives in NVS next to the wifi
 * credentials. Empty means "use whatever was built in".
 *
 * What gets typed is an address, not a URL: the on-screen keyboard has digits,
 * a dot and a colon, but no slash. So "192.168.1.42" and "192.168.1.42:9000"
 * both work, and a full "ws://host:port/path" is accepted too for anyone
 * pasting one in over serial. */
void net_get_proxy_host(char *out, size_t len)
{
    if (!len) return;
    out[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t n = len;
    if (nvs_get_str(h, NVS_PROXY, out, &n) != ESP_OK) out[0] = '\0';
    nvs_close(h);
}

void net_get_proxy_url(char *out, size_t len)
{
    char host[96];
    net_get_proxy_host(host, sizeof(host));

    if (!host[0]) { strlcpy(out, BEEBO_PROXY_URL, len); return; }
    if (strstr(host, "://")) { strlcpy(out, host, len); return; }

    /* A colon in the tail means they gave a port; an IPv6 literal would have
     * more than one, and is not worth supporting on this keyboard. */
    const char *colon = strrchr(host, ':');
    if (colon && colon != host && strchr(host, ':') == colon)
        snprintf(out, len, "ws://%s/ws", host);
    else
        snprintf(out, len, "ws://%s:%d/ws", host, BEEBO_PROXY_PORT);
}

esp_err_t net_set_proxy_host(const char *text)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (text && text[0]) err = nvs_set_str(h, NVS_PROXY, text);
    else                 nvs_erase_key(h, NVS_PROXY);   /* back to the built-in */
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "proxy address set to \"%s\"", text && text[0] ? text : "(built-in)");
    return err;
}

#define MAX_APS 24
static wifi_ap_record_t s_aps[MAX_APS];

/* This runs on the system event task. It used to sleep two seconds here and
 * call esp_wifi_connect() inline, which is why the board never came back from
 * a nap: blocking this task stalls every other event behind it, including the
 * disconnect that the retry itself provokes, so the queue backs up and the
 * chain quietly dies. Nothing in here may block - the supervisor does the
 * waiting. */
static void ps_apply(void);

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        s_link_up = false;
        xEventGroupClearBits(s_evt, BIT_GOT_IP);
        ps_apply();                       /* full power for the reconnect */
        if (s_want_connect) {
            ESP_LOGW(TAG, "wifi dropped (reason %d) - retrying", d ? d->reason : 0);
            xEventGroupSetBits(s_evt, BIT_RETRY);
            ui_set_wifi_state(false);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_link_up = true;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_evt, BIT_GOT_IP);
        ps_apply();                       /* honour a nap requested while down */
        ui_set_wifi_state(true);
        /* The socket backs off on its own schedule; tell it the network is
         * back rather than making it wait out a retry it started while there
         * was nothing to connect to. */
        voice_kick();
    }
}

/* Keeps the link alive for the life of the board. The old arrangement had a
 * boot task that connected once and deleted itself, so any drop after boot -
 * a nap, an AP reboot, walking out of range - was permanent. */
static void supervisor_task(void *arg)
{
    /* Starts short deliberately. The common drop at boot is an auth timeout
     * that succeeds on an immediate retry, and a two-second first backoff
     * turned a four-second association into twenty-five. */
    int backoff_ms = 500, dry = 0;

    for (;;) {
        /* Wake on a drop, or look around every 10 s anyway: a link can go
         * away without the driver ever raising an event. */
        xEventGroupWaitBits(s_evt, BIT_RETRY, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));

        if (!s_want_connect) { backoff_ms = 500; dry = 0; continue; }
        if (xEventGroupGetBits(s_evt) & BIT_GOT_IP) { backoff_ms = 500; dry = 0; continue; }

        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        if (!s_want_connect || (xEventGroupGetBits(s_evt) & BIT_GOT_IP)) continue;

        if (++dry % 8 == 0) {
            /* Eight failures deep the driver itself is usually the problem -
             * a full stop/start clears states that esp_wifi_connect() alone
             * will not. */
            ESP_LOGW(TAG, "wifi still down after %d tries - restarting the driver", dry);
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_wifi_start();
        }
        ESP_LOGI(TAG, "wifi reconnect attempt %d", dry);
        esp_wifi_connect();

        backoff_ms = backoff_ms < 15000 ? backoff_ms * 2 : 15000;
    }
}

esp_err_t net_init(void)
{
    s_evt = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* Starts awake; the idle path turns modem sleep on once nothing
     * is streaming. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    known_load();
    esp_err_t err = esp_wifi_start();
    xTaskCreate(supervisor_task, "wifisup", 3072, NULL, 4, NULL);
    return err;
}

bool net_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    /* The remembered list is authoritative and is ordered by what worked most
     * recently; the loose pair is what older firmware wrote. */
    if (s_known_n) {
        strlcpy(ssid, s_known[0].ssid, ssid_len);
        strlcpy(pass, s_known[0].pass, pass_len);
        return true;
    }
    if (read_pair(NVS_NS, ssid, ssid_len, pass, pass_len) == ESP_OK) return true;
    if (read_pair(NVS_NS_LEGACY, ssid, ssid_len, pass, pass_len) == ESP_OK) return true;
    /* fall back to whatever was baked in at build time */
    if (BEEBO_WIFI_SSID[0]) {
        strlcpy(ssid, BEEBO_WIFI_SSID, ssid_len);
        strlcpy(pass, BEEBO_WIFI_PASS, pass_len);
        return true;
    }
    return false;
}

esp_err_t net_save_creds(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass);
    err = nvs_commit(h);
    nvs_close(h);

    /* Read it back: a commit that silently did not stick is the difference
     * between "works until you unplug it" and actually working. */
    char check[33] = "";
    size_t n = sizeof(check);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_SSID, check, &n);
        nvs_close(h);
    }
    if (strcmp(check, ssid)) {
        ESP_LOGE(TAG, "credential write did NOT persist (read back \"%s\")", check);
        return ESP_FAIL;
    }
    known_remember(ssid, pass);
    ESP_LOGI(TAG, "saved credentials for \"%s\" (verified)", ssid);
    return err;
}

void net_clear_creds(void)
{
    s_known_n = 0;
    memset(s_known, 0, sizeof(s_known));
    static const char *spaces[] = { NVS_NS, NVS_NS_LEGACY };
    for (size_t i = 0; i < sizeof(spaces) / sizeof(spaces[0]); i++) {
        nvs_handle_t h;
        if (nvs_open(spaces[i], NVS_READWRITE, &h) != ESP_OK) continue;
        nvs_erase_key(h, NVS_SSID);
        nvs_erase_key(h, NVS_PASS);
        nvs_erase_key(h, NVS_KNOWN);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t net_connect(const char *ssid, const char *pass)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass, sizeof(wc.sta.password));
    /* Leave authmode at OPEN so open and WEP networks still work; the driver
     * negotiates whatever the AP actually offers. */
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;

    xEventGroupClearBits(s_evt, BIT_GOT_IP);
    s_want_connect = true;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    return esp_wifi_connect();
}

bool net_wait_ip(int timeout_ms)
{
    return (xEventGroupWaitBits(s_evt, BIT_GOT_IP, pdFALSE, pdTRUE,
                                pdMS_TO_TICKS(timeout_ms)) & BIT_GOT_IP) != 0;
}

void net_get_ip(char *out, size_t len)
{
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(s_netif, &ip);
    snprintf(out, len, IPSTR, IP2STR(&ip.ip));
}

esp_err_t net_scan(wifi_ap_record_t **records, uint16_t *count)
{
    s_want_connect = false;               /* don't fight the scan with retries */
    esp_wifi_disconnect();

    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* blocking */
    if (err != ESP_OK) return err;

    uint16_t n = MAX_APS;
    err = esp_wifi_scan_get_ap_records(&n, s_aps);
    if (err != ESP_OK) return err;

    *records = s_aps;
    *count   = n;
    ESP_LOGI(TAG, "scan found %u APs", n);
    return ESP_OK;
}

/* Modem sleep is only safe once there is a link. Associating with it enabled
 * fails over and over with reason 2 (auth timeout): the radio is asleep for
 * the handshake it is supposed to be having. That is what made a nap look
 * permanent - the nap turned power save on, the link eventually dropped, and
 * every reconnect attempt after that was made with the radio half-asleep.
 *
 * So the request is remembered and applied only while associated; a drop puts
 * the radio back at full power until an address comes back. */
static bool s_ps_want, s_ps_cur;

static void ps_apply(void)
{
    bool want = s_ps_want && s_link_up;
    if (want == s_ps_cur) return;
    if (esp_wifi_set_ps(want ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE) != ESP_OK) return;
    s_ps_cur = want;
    ESP_LOGI(TAG, "wifi power save %s", want ? "on" : "off");
}

void net_set_power_save(bool on)
{
    s_ps_want = on;
    ps_apply();
}

int net_get_rssi(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void net_get_ssid(char *out, size_t len)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) strlcpy(out, (const char *)ap.ssid, len);
    else if (len) out[0] = '\0';
}

/* TLS certificate validation fails outright if the clock is at 1970, so this
 * has to land before any wss:// connection - but plain ws:// on the LAN does
 * not care, so it runs in the background rather than gating boot. Several
 * servers are listed because reachability varies a lot by network. */
void net_sntp_start(void)
{
    /* Alibaba's numbered servers first: they answer reliably from mainland
     * networks, where pool.ntp.org times out often enough to matter. */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(
        3, ESP_SNTP_SERVER_LIST("ntp1.aliyun.com", "ntp2.aliyun.com", "pool.ntp.org"));
    cfg.start = true;
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        ESP_LOGW(TAG, "sntp init: %s", esp_err_to_name(err));
    setenv("TZ", BEEBO_TZ, 1);
    tzset();
}

bool net_time_valid(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    return tm.tm_year > (2020 - 1900);
}
