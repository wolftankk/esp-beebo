#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_wifi.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Brings up netif + the wifi driver in STA mode without connecting. */
esp_err_t net_init(void);

/* Credentials live in NVS so they survive reflashing the app partition.
 * Falls back to the Kconfig values when NVS is empty. */
bool      net_load_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
esp_err_t net_save_creds(const char *ssid, const char *pass);
void      net_clear_creds(void);

esp_err_t net_connect(const char *ssid, const char *pass);

/* True once the link has an address, and false the moment it drops. Cheaper
 * and more current than asking the driver. */
bool      net_is_up(void);

/* The password last used for this SSID, if the board has ever connected to it.
 * The scanner uses this so picking a familiar network just works instead of
 * asking for a password the board already knows. */
bool      net_known_pass(const char *ssid, char *pass, size_t len);
/* Drops a remembered network, so the next tap asks for the password again. */
void      net_forget(const char *ssid);
bool      net_wait_ip(int timeout_ms);
void      net_get_ip(char *out, size_t len);

/* Blocking scan. Caller frees nothing; the records live in a static buffer
 * valid until the next scan. */
esp_err_t net_scan(wifi_ap_record_t **records, uint16_t *count);

/* Live link info; rssi returns 0 and ssid is emptied when not associated. */
int       net_get_rssi(void);
void      net_get_ssid(char *out, size_t len);

/* Kicks SNTP off and returns immediately. Blocking the boot on it cost 15 s
 * and still failed - pool.ntp.org is not dependable from every network, and
 * nothing before the gateway handshake actually needs the clock. */
/* Modem sleep between beacons. Off during a voice turn, where the radio
 * latency would show up as stutter; on whenever the board is idle. */
void      net_set_power_save(bool on);

void      net_sntp_start(void);
bool      net_time_valid(void);

#ifdef __cplusplus
}
#endif
