#pragma once
// The two I2C chips between the ESP32-S3 and the panel (private to the board
// component): the AXP2101 PMIC that feeds its rails, and the TCA9554 IO expander
// that holds its power-enable and reset lines.
#include "esp_err.h"
#include "driver/i2c_master.h"

esp_err_t pmic_init(i2c_master_bus_handle_t bus);            // DC1 and ALDO1 at 3.3 V, the rest off
esp_err_t expander_panel_reset(i2c_master_bus_handle_t bus); // panel power on, one reset pulse
