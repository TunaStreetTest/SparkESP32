// SparkESP32 — a vitals display and power light for the NVIDIA DGX Spark.
//
// An agent on the Spark samples nvidia-smi + /proc once a second and publishes
// a `stat` line two ways: pushed down USB serial (board plugged into the
// Spark) and served at GET /stat (board on the same Wi-Fi LAN, polled by
// net.c). USB wins while fresh. When the feed stops, the screen says so.

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#include "board.h"
#include "console_cmds.h"
#include "display.h"
#include "net.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "main";

// 1 Hz repaint so the liveness state ages even when no samples arrive
static void tick_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (display_lock(500) == ESP_OK) {
            ui_update();
            display_unlock();
        }
    }
}

static display_rot_t saved_rotation(void)
{
    char r[8];
    settings_get("rot", r, sizeof(r), "cw");   // cw: upright with the USB-C on the right
    if (!strcmp(r, "none")) return DISPLAY_ROT_NONE;
    if (!strcmp(r, "ccw")) return DISPLAY_ROT_CCW;
    return DISPLAY_ROT_CW;
}

void app_main(void)
{
    // ESP32-C6 erratum (espressif/esp-idf#17432): a power-on boot that races the
    // host enumerating the USB port can leave the USB-Serial/JTAG link dead, with
    // no bytes either way, until the chip is reset. A CPU reset after enumeration
    // always brings it up and never breaks it, and esp_restart() on the C6 resets
    // only the CPU, so the host keeps the port. Restart once per power-on (~0.5 s).
    if (esp_reset_reason() == ESP_RST_POWERON) {
        esp_restart();
    }

    ESP_ERROR_CHECK(settings_init());
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(display_start(saved_rotation()));

    char name[25];
    settings_get("name", name, sizeof(name), "");
    if (display_lock(-1) == ESP_OK) {
        ui_init(name);
        ui_update();
        display_unlock();
    }

    net_start();
    xTaskCreatePinnedToCore(tick_task, "tick", 6144, NULL, 5, NULL, 0);
    console_start();
    ESP_LOGI(TAG, "SparkESP32 up");
}
