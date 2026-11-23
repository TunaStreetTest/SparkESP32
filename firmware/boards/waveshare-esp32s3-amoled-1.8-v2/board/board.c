// Board bring-up: I2C bus -> AXP2101 rails -> TCA9554 (panel power and reset) ->
// QSPI AMOLED panel. A dark panel after a flash is nearly always this order.

#include "board.h"
#include "board_lcd.h"
#include "pmic.h"

#include <stdlib.h>

#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"

static const char *TAG = "board";

#define PIN_I2C_SCL   14
#define PIN_I2C_SDA   15
#define PIN_LCD_CS    12
#define PIN_LCD_SCLK  11
#define PIN_LCD_D0    4
#define PIN_LCD_D1    5
#define PIN_LCD_D2    6
#define PIN_LCD_D3    7

#define LCD_HOST      SPI2_HOST
#define LCD_PCLK_HZ   (40 * 1000 * 1000)
#define LCD_MAX_XFER  (BOARD_STRIP_PIXELS * 2 + 64)   // one LVGL strip in a single transfer
#define LCD_GAP_X     16      // the 368 visible columns start at column 16 of the controller's RAM

static i2c_master_bus_handle_t   s_i2c;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t    s_panel;

// Panel init sequence (DCS + vendor page registers).
static const co5300_lcd_init_cmd_t s_lcd_init[] = {
    {0x11, (uint8_t[]){0x00}, 0, 600},                    // SLPOUT, then let the panel settle
    {0xFE, (uint8_t[]){0x20}, 1, 0},                      // vendor register page 0x20
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0},                      // back to the user command page
    {0xC4, (uint8_t[]){0x80}, 1, 0},                      // QSPI interface mode
    {0x3A, (uint8_t[]){0x55}, 1, 0},                      // COLMOD: RGB565
    {0x35, (uint8_t[]){0x00}, 1, 0},                      // tearing-effect line on
    {0x53, (uint8_t[]){0x20}, 1, 0},                      // brightness control on
    {0x51, (uint8_t[]){0xFF}, 1, 0},                      // brightness: max
    {0x63, (uint8_t[]){0xFF}, 1, 0},                      // HBM brightness: max
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},    // CASET
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xDF}, 4, 0},    // RASET
    {0x36, (uint8_t[]){0x00}, 1, 0},                      // MADCTL
};

static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_NUM_0,
        .scl_io_num = PIN_I2C_SCL,
        .sda_io_num = PIN_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

// The controller's RAM holds whatever it powered up with: black it out before the
// panel is switched on, so that never shows.
static esp_err_t lcd_clear(void)
{
    const int rows = 32;
    uint16_t *black = heap_caps_calloc(BOARD_LCD_W * rows, 2, MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(black, ESP_ERR_NO_MEM, TAG, "clear buffer");
    esp_err_t err = ESP_OK;
    for (int y = 0; y < BOARD_LCD_H && err == ESP_OK; y += rows)
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_LCD_W, y + rows, black);
    // Switching the panel on sends a command, which waits for the last strip's DMA:
    // only then is the buffer free to go.
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    free(black);
    return err;
}

static esp_err_t lcd_init(void)
{
    spi_bus_config_t bus = {
        .sclk_io_num = PIN_LCD_SCLK,
        .data0_io_num = PIN_LCD_D0,
        .data1_io_num = PIN_LCD_D1,
        .data2_io_num = PIN_LCD_D2,
        .data3_io_num = PIN_LCD_D3,
        .max_transfer_sz = LCD_MAX_XFER,
        .flags = SPICOMMON_BUSFLAG_QUAD,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags.quad_mode = true,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io, &s_io),
                        TAG, "panel io");

    co5300_vendor_config_t vendor = {
        .init_cmds = s_lcd_init,
        .init_cmds_size = sizeof(s_lcd_init) / sizeof(s_lcd_init[0]),
        .flags.use_qspi_interface = 1,
    };
    esp_lcd_panel_dev_config_t dev = {
        .reset_gpio_num = -1,                // the reset line is on the IO expander
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_io, &dev, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, LCD_GAP_X, 0), TAG, "panel gap");
    return lcd_clear();
}

esp_err_t board_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(pmic_init(s_i2c), TAG, "pmic");
    ESP_RETURN_ON_ERROR(expander_panel_reset(s_i2c), TAG, "expander");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd");
    ESP_LOGI(TAG, "board up: %dx%d AMOLED", BOARD_LCD_W, BOARD_LCD_H);
    return ESP_OK;
}

esp_lcd_panel_handle_t board_lcd_panel(void) { return s_panel; }
esp_lcd_panel_io_handle_t board_lcd_io(void) { return s_io; }
