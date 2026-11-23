#pragma once
// Private to this board: the panel handles board.c creates and display.c drives.

#include "esp_lcd_types.h"

// The panel's own shape: portrait.
#define BOARD_LCD_W 368
#define BOARD_LCD_H 448

// LVGL renders in strips of at most this many pixels: 40 rows of the landscape
// canvas, 48 of the portrait one. board.c sizes the SPI transfer for one strip.
#define BOARD_STRIP_PIXELS (BOARD_LCD_H * 40)

esp_lcd_panel_handle_t    board_lcd_panel(void);
esp_lcd_panel_io_handle_t board_lcd_io(void);
