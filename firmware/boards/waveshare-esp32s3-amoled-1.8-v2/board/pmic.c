// AXP2101 and TCA9554, by plain register writes: each needs a handful, so neither
// gets a driver. The values are the ones the board's published firmware uses (see
// the board README for the sources).

#include "pmic.h"

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pmic";

#define AXP2101_ADDR   0x34
#define TCA9554_ADDR   0x20
#define I2C_HZ         100000
#define I2C_TIMEOUT_MS 100
#define I2C_TRIES      3

static esp_err_t add_device(i2c_master_bus_handle_t bus, uint8_t addr, i2c_master_dev_handle_t *dev)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = I2C_HZ,
    };
    return i2c_master_bus_add_device(bus, &cfg, dev);
}

static esp_err_t reg_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = {reg, val};
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < I2C_TRIES; i++) {
        err = i2c_master_transmit(dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
        if (err == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return err;
}

// AXP2101 registers
#define AXP_PWROFF_EN     0x22    // what may switch the board off
#define AXP_KEY_TIMES     0x27    // how long the PWR key is held for an IRQ, and for off
#define AXP_DCDC_EN       0x80
#define AXP_DCDC1_VOLT    0x82    // (mV - 1500) / 100
#define AXP_LDO_EN0       0x90    // bit 0: ALDO1
#define AXP_LDO_EN1       0x91
#define AXP_ALDO1_VOLT    0x92    // (mV - 500) / 100

esp_err_t pmic_init(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t axp;
    ESP_RETURN_ON_ERROR(add_device(bus, AXP2101_ADDR, &axp), TAG, "add AXP2101");
    const struct { uint8_t reg, val; } writes[] = {
        {AXP_PWROFF_EN, 0x06},
        {AXP_KEY_TIMES, 0x10},
        {AXP_DCDC_EN, 0x01},                        // DC1 only: it runs the ESP32-S3
        {AXP_LDO_EN0, 0x00},                        // every LDO off...
        {AXP_LDO_EN1, 0x00},
        {AXP_DCDC1_VOLT, (3300 - 1500) / 100},
        {AXP_ALDO1_VOLT, (3300 - 500) / 100},
        {AXP_LDO_EN0, 0x01},                        // ...then ALDO1 on, at 3.3 V
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++)
        ESP_RETURN_ON_ERROR(reg_write(axp, writes[i].reg, writes[i].val), TAG, "AXP2101 reg 0x%02x",
                            writes[i].reg);
    ESP_LOGI(TAG, "AXP2101 up");
    return ESP_OK;
}

// TCA9554 registers, and the three lines this board hangs on it
#define TCA_OUTPUT        0x01
#define TCA_CONFIG        0x03    // 1 = input
#define TCA_LCD_RESET     (1 << 0)
#define TCA_PANEL_POWER   (1 << 1)
#define TCA_TOUCH_RESET   (1 << 2)
#define TCA_LINES         (TCA_LCD_RESET | TCA_PANEL_POWER | TCA_TOUCH_RESET)

esp_err_t expander_panel_reset(i2c_master_bus_handle_t bus)
{
    i2c_master_dev_handle_t tca;
    ESP_RETURN_ON_ERROR(add_device(bus, TCA9554_ADDR, &tca), TAG, "add TCA9554");
    // set the levels before the pins become outputs, so they don't start low
    ESP_RETURN_ON_ERROR(reg_write(tca, TCA_OUTPUT, TCA_LINES), TAG, "TCA9554 output");
    ESP_RETURN_ON_ERROR(reg_write(tca, TCA_CONFIG, (uint8_t)~TCA_LINES), TAG, "TCA9554 config");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(reg_write(tca, TCA_OUTPUT, 0x00), TAG, "TCA9554 low");
    vTaskDelay(pdMS_TO_TICKS(300));
    ESP_RETURN_ON_ERROR(reg_write(tca, TCA_OUTPUT, TCA_LINES), TAG, "TCA9554 high");
    vTaskDelay(pdMS_TO_TICKS(120));                 // the panel needs ~100 ms after power-up
    ESP_LOGI(TAG, "TCA9554: panel powered and reset");
    return ESP_OK;
}
