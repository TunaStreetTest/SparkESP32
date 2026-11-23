idf_component_register(
    SRCS "board.c" "pmic.c" "display.c"
    INCLUDE_DIRS "include"
    REQUIRES board_api
    PRIV_REQUIRES
        esp_driver_i2c
        esp_driver_spi
        esp_lcd
        esp_timer
        lvgl__lvgl
        espressif__esp_lcd_co5300)
