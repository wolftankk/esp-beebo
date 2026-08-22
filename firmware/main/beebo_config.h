#pragma once
#include "sdkconfig.h"

/*
 * Where the board's four deployment settings actually come from.
 *
 * Precedence is environment > menuconfig > default, and the reason is that a
 * public repository must not carry anybody's network in it. `sdkconfig` is
 * generated and gitignored; what is committed are placeholder defaults. Point
 * the build at your own by exporting the variables:
 *
 *     export BEEBO_PROXY_URL="ws://192.168.1.42:18797/ws"
 *     export BEEBO_WIFI_SSID="..." BEEBO_WIFI_PASS="..."
 *     idf.py build
 *
 * CMake reads the environment at configure time only, so changing a variable
 * after a build needs `idf.py reconfigure` to take effect.
 *
 * WiFi credentials are optional here and usually better left unset: the board
 * has a scanner and a keyboard in settings, and what you enter there lands in
 * NVS instead of in the binary.
 */

#ifdef BEEBO_PROXY_URL_ENV
#define BEEBO_PROXY_URL    BEEBO_PROXY_URL_ENV
#else
#define BEEBO_PROXY_URL    CONFIG_BEEBO_PROXY_URL
#endif

#ifdef BEEBO_PROXY_TOKEN_ENV
#define BEEBO_PROXY_TOKEN  BEEBO_PROXY_TOKEN_ENV
#else
#define BEEBO_PROXY_TOKEN  CONFIG_BEEBO_PROXY_TOKEN
#endif

#ifdef BEEBO_WIFI_SSID_ENV
#define BEEBO_WIFI_SSID    BEEBO_WIFI_SSID_ENV
#else
#define BEEBO_WIFI_SSID    CONFIG_BEEBO_WIFI_SSID
#endif

#ifdef BEEBO_WIFI_PASS_ENV
#define BEEBO_WIFI_PASS    BEEBO_WIFI_PASS_ENV
#else
#define BEEBO_WIFI_PASS    CONFIG_BEEBO_WIFI_PASS
#endif

#ifdef BEEBO_TZ_ENV
#define BEEBO_TZ           BEEBO_TZ_ENV
#else
#define BEEBO_TZ           CONFIG_BEEBO_TZ
#endif

/* Used when the address typed on the device omits a port. */
#ifndef BEEBO_PROXY_PORT
#define BEEBO_PROXY_PORT   18797
#endif

/* Identifies this board to the proxy. Not a secret and not unique - the proxy
 * keys sessions on it, so change it if you run more than one. */
#ifdef BEEBO_DEVICE_ID_ENV
#define BEEBO_DEVICE_ID    BEEBO_DEVICE_ID_ENV
#else
#define BEEBO_DEVICE_ID    "beebo"
#endif
