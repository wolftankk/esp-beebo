/*
 * Waveshare ESP32-S3-Touch-LCD-3.49  —  verified pin map
 * Sources: Waveshare official examples (11_FactoryProgram/main/user_config.h,
 *          codec_board/board_cfg.txt entry "S3_LCD_3_49",
 *          04_SD_Card/components/sdcard_bsp/sdcard_bsp.c)
 *
 * SoC: ESP32-S3R8 (8MB octal PSRAM) + 16MB flash (W25Q128)
 * No pin conflicts between LCD / touch / audio / SD — all on separate GPIOs.
 */
#pragma once
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"

/* ---- LCD: AXS15231B, QSPI ---- */
#define BSP_LCD_HOST         SPI3_HOST
#define BSP_LCD_H_RES        172
#define BSP_LCD_V_RES        640
#define BSP_PIN_LCD_CS       GPIO_NUM_9
#define BSP_PIN_LCD_PCLK     GPIO_NUM_10
#define BSP_PIN_LCD_D0       GPIO_NUM_11
#define BSP_PIN_LCD_D1       GPIO_NUM_12
#define BSP_PIN_LCD_D2       GPIO_NUM_13
#define BSP_PIN_LCD_D3       GPIO_NUM_14
#define BSP_PIN_LCD_RST      GPIO_NUM_21
#define BSP_PIN_LCD_BL       GPIO_NUM_8

/* Full frame = 172*640*2 = 220160 B (PSRAM). DMA chunk = 1/10 of a frame. */
#define BSP_LCD_FRAME_BYTES  (BSP_LCD_H_RES * BSP_LCD_V_RES * 2)
#define BSP_LCD_DMA_BYTES    (BSP_LCD_H_RES * 64 * 2)

/* ---- Touch: AXS15231B integrated controller, its own I2C bus ---- */
#define BSP_TOUCH_I2C_PORT   I2C_NUM_1
#define BSP_PIN_TOUCH_SDA    GPIO_NUM_17
#define BSP_PIN_TOUCH_SCL    GPIO_NUM_18
#define BSP_TOUCH_ADDR       0x3B

/* ---- System I2C: TCA9554 expander, PCF85063 RTC, QMI8658 IMU, ES8311, ES7210 ---- */
#define BSP_SYS_I2C_PORT     I2C_NUM_0
#define BSP_PIN_SYS_SDA      GPIO_NUM_47
#define BSP_PIN_SYS_SCL      GPIO_NUM_48
#define BSP_ADDR_RTC         0x51
#define BSP_ADDR_IMU         0x6B

/* ---- I2S audio: ES8311 (DAC/out) + ES7210 (ADC/mic array, in) ---- */
#define BSP_PIN_I2S_MCLK     GPIO_NUM_7
#define BSP_PIN_I2S_BCLK     GPIO_NUM_15
#define BSP_PIN_I2S_WS       GPIO_NUM_46
#define BSP_PIN_I2S_DIN      GPIO_NUM_6
#define BSP_PIN_I2S_DOUT     GPIO_NUM_45
/* Board has no PA-enable GPIO (codec_board: pa = -1) */

/* ---- TF card: SDMMC slot 1, 1-bit, dedicated pins (does NOT share the LCD bus) ---- */
#define BSP_PIN_SD_CLK       GPIO_NUM_41
#define BSP_PIN_SD_CMD       GPIO_NUM_39
#define BSP_PIN_SD_D0        GPIO_NUM_40

/* ---- Buttons (both back side, active low) ----
 * BOOT is only special while RESET is released; at runtime it is an ordinary
 * input, so it is usable as the push-to-talk key. */
#define BSP_PIN_BTN_BOOT     GPIO_NUM_0
#define BSP_PIN_BTN_PWR      GPIO_NUM_16
