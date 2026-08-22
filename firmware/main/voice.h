#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t voice_init(void);
/* Opens the connection. Separate from init so it happens once there is a
 * network to open it on - starting earlier just logged failures. */
esp_err_t voice_connect(void);

/* The network came back. Cuts short whatever backoff the client is sitting in,
 * since that backoff was measured against a network that did not exist. Safe
 * to call when already connected, and safe before voice_connect(). */
void      voice_kick(void);

/* The proxy address changed in settings. Re-reads it and reopens the socket. */
void      voice_reconnect(void);
/* Push-to-talk edges. Capture runs on its own task and meters the level into
 * the robot's grille; releasing hands the clip off for the round trip. */
void voice_start(void);
void voice_stop(void);
esp_err_t voice_start_capture_task(void);

#ifdef __cplusplus
}
#endif
