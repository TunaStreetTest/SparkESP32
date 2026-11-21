#pragma once
// Board support for the Waveshare ESP32-C6-Touch-AMOLED-2.16 (480x480 AMOLED,
// SH8601-family controller on QSPI, AXP2101 PMIC on I2C). Display only: the
// CST9217 touch controller is present but unused by SparkESP32.

#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_LCD_W 480
#define BOARD_LCD_H 480

// Bring up I2C, the PMIC rails, and the LCD panel. Call once, first thing.
esp_err_t board_init(void);

esp_lcd_panel_handle_t    board_lcd_panel(void);
esp_lcd_panel_io_handle_t board_lcd_io(void);

// Panel brightness, 0-100 %.
esp_err_t board_set_brightness(uint8_t pct);

#ifdef __cplusplus
}
#endif
