#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

/* QMI8658 six-axis. Started after the codec, because it shares the system I2C
 * bus that codec_board stands up. */
esp_err_t imu_start(void);
bool      imu_present(void);

/* Lean of the board away from upright, in degrees. Positive is one way,
 * negative the other; only the sign convention matters and the face uses it
 * directly. Zero when there is no IMU, so callers need no special case. */
int       imu_roll_deg(void);

#ifdef __cplusplus
}
#endif
