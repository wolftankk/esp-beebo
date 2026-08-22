#pragma once
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

/* PCF85063 real-time clock.
 *
 * The point of it is that the board knows what time it is before it knows
 * whether it has a network. Boot used to show "--:--" for however long the
 * association took - measured on this desk, anywhere from eight to thirty-four
 * seconds - and forever if wifi never came up at all.
 *
 * So the RTC is the clock, and NTP is only ever a correction to it. */
esp_err_t rtc_start(void);          /* after the codec: shares its I2C bus */
bool      rtc_present(void);

/* Reads the chip into the system clock. False if the RTC has never been set or
 * lost its oscillator, in which case the system clock is left alone. */
bool      rtc_restore_system_time(void);

/* Writes the system clock into the chip. Called when NTP lands. */
esp_err_t rtc_save_system_time(void);

#ifdef __cplusplus
}
#endif
