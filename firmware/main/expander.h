#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

/* TCA9554 at 0x20. From the board schematic's pin table:
 *   EXIO0 TP_INT   EXIO1 BL_EN    EXIO2 IMU_INT1  EXIO3 IMU_INT2
 *   EXIO4 RTC_INT  EXIO5 LCD_TE   EXIO6 SYS_EN    EXIO7 NS_MODE
 */
#define EXIO_SYS_EN   6   /* latches the battery power path on            */
#define EXIO_NS_MODE  7   /* CTRL of the NS4150B amplifier, 10K pulled low */

/* Drives one expander pin as an output. Safe to call before or after the
 * codec claims the bus: it reuses codec_board's handle when one exists and
 * borrows the bus briefly otherwise. */
esp_err_t expander_set(int pin, bool high);

#ifdef __cplusplus
}
#endif
