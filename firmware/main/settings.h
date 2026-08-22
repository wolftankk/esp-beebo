#pragma once
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  volume;      /* 0..100, applied to ES8311 once audio lands */
    uint8_t  brightness;  /* 0..255 backlight duty                     */
    uint8_t  font;        /* 0 = small(14), 1 = medium(18), 2 = large(24) */
    uint16_t doze_sec;    /* nod off after this many idle seconds; 0 = never */
} settings_t;

settings_t *settings_get(void);
void        settings_load(void);   /* NVS -> struct, with defaults */
void        settings_save(void);          /* coalesced write, 1.5 s window */
void        settings_save_blocking(void); /* commit right now              */
void        settings_apply(void);  /* push values into the hardware/UI */

#ifdef __cplusplus
}
#endif
