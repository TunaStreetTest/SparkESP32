// LVGL on the CO5300 AMOLED, in either shape.
//
// The panel is 368x448, portrait, and its controller can't exchange rows and
// columns. So LVGL renders the canvas the viewer sees (368x448 for `rot none`,
// 448x368 for `rot cw` and `rot ccw`) in strips, and the flush turns each strip
// into the panel's own orientation on its way into a DMA buffer, byte-swapping to
// the big-endian RGB565 the panel reads in the same pass. LVGL's own buffer is
// never sent, so it lives in PSRAM.

#include "display.h"
#include "board.h"
#include "board_lcd.h"

#include <assert.h>
#include <stdbool.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#define STRIP_BYTES (BOARD_STRIP_PIXELS * 2)

static const char *TAG = "display";
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_sent;            // given when a strip's DMA has finished
static lv_display_t *s_disp;
static uint16_t *s_tx;                      // one strip, as the panel wants it
static display_rot_t s_rot = DISPLAY_ROT_CW;

static bool tall(void) { return s_rot == DISPLAY_ROT_NONE; }

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// The SPI ISR only signals: LVGL is not safe to call from here (an ISR that runs
// while flash is busy, saving settings, can't call code that lives in flash).
static bool IRAM_ATTR strip_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woke = pdFALSE;
    xSemaphoreGiveFromISR(s_sent, &woke);
    return woke == pdTRUE;
}

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px_map)
{
    const int w = lv_area_get_width(a);
    const int h = lv_area_get_height(a);
    assert(w * h <= BOARD_STRIP_PIXELS);    // an area never holds more pixels than the draw buffer
    // LVGL may pad draw-buffer rows (LV_DRAW_BUF_STRIDE_ALIGN): walk the source at
    // its real stride, not at w.
    const int stride = (int)lv_draw_buf_width_to_stride(w, LV_COLOR_FORMAT_RGB565) / 2;
    const uint16_t *src = (const uint16_t *)px_map;
    int x1, y1, x2, y2;                     // the strip, in the panel's coordinates
    if (s_rot == DISPLAY_ROT_NONE) {
        x1 = a->x1; x2 = a->x2; y1 = a->y1; y2 = a->y2;
        for (int y = 0; y < h; y++) {
            const uint16_t *row = src + y * stride;
            uint16_t *out = s_tx + y * w;
            for (int x = 0; x < w; x++) out[x] = __builtin_bswap16(row[x]);
        }
    } else if (s_rot == DISPLAY_ROT_CCW) {
        // canvas (x,y) -> panel (y, H-1-x); the strip goes out h wide, w tall
        x1 = a->y1; x2 = a->y2;
        y1 = (BOARD_LCD_H - 1) - a->x2; y2 = (BOARD_LCD_H - 1) - a->x1;
        for (int y = 0; y < h; y++) {
            const uint16_t *row = src + y * stride;
            uint16_t *col = s_tx + y;
            for (int x = 0; x < w; x++) col[(w - 1 - x) * h] = __builtin_bswap16(row[x]);
        }
    } else {
        // canvas (x,y) -> panel (W-1-y, x)
        x1 = (BOARD_LCD_W - 1) - a->y2; x2 = (BOARD_LCD_W - 1) - a->y1;
        y1 = a->x1; y2 = a->x2;
        for (int y = 0; y < h; y++) {
            const uint16_t *row = src + y * stride;
            uint16_t *col = s_tx + (h - 1 - y);
            for (int x = 0; x < w; x++) col[x * h] = __builtin_bswap16(row[x]);
        }
    }
    if (esp_lcd_panel_draw_bitmap(board_lcd_panel(), x1, y1, x2 + 1, y2 + 1, s_tx) == ESP_OK)
        xSemaphoreTake(s_sent, pdMS_TO_TICKS(1000));   // the DMA reads s_tx until this
    lv_display_flush_ready(d);
}

// The CO5300 needs window starts on even and ends on odd coordinates. Both of the
// panel's sides are even, so that parity survives the remap above. LVGL also asks
// this when it sizes a strip, which keeps every strip an even number of rows.
static void round_area_cb(lv_event_t *e)
{
    lv_area_t *a = (lv_area_t *)lv_event_get_param(e);
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

static void lvgl_task(void *arg)
{
    while (1) {
        uint32_t wait = 100;
        if (display_lock(-1) == ESP_OK) {
            wait = lv_timer_handler();
            display_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(wait < 5 ? 5 : wait > 100 ? 100 : wait));
    }
}

esp_err_t display_start(display_rot_t rot)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    s_sent = xSemaphoreCreateBinary();
    s_tx = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    void *buf = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_SPIRAM);
    if (!buf) buf = heap_caps_malloc(STRIP_BYTES, MALLOC_CAP_DEFAULT);
    if (!s_lock || !s_sent || !s_tx || !buf) return ESP_ERR_NO_MEM;

    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = strip_sent};
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_register_event_callbacks(board_lcd_io(), &cbs, NULL), TAG, "callbacks");
    s_rot = rot;

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(tall() ? BOARD_LCD_W : BOARD_LCD_H, tall() ? BOARD_LCD_H : BOARD_LCD_W);
    if (!s_disp) return ESP_FAIL;
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, buf, NULL, STRIP_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_display_add_event_cb(s_disp, round_area_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    return xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 5, NULL, 1) == pdPASS ? ESP_OK : ESP_FAIL;
}

esp_err_t display_lock(int timeout_ms)
{
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, t) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

void display_unlock(void) { xSemaphoreGiveRecursive(s_lock); }

// All three work: `none` is the panel's own portrait, `cw` and `ccw` are landscape,
// one the other turned 180 degrees. A change of shape changes LVGL's resolution,
// and the caller lays the UI out again and redraws everything.
esp_err_t display_set_rotation(display_rot_t rot)
{
    s_rot = rot;
    if (s_disp)
        lv_display_set_resolution(s_disp, tall() ? BOARD_LCD_W : BOARD_LCD_H, tall() ? BOARD_LCD_H : BOARD_LCD_W);
    return ESP_OK;
}

display_rot_t display_get_rotation(void) { return s_rot; }
