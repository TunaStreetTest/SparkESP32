#pragma once
// LVGL on top of the board's panel, via espressif/esp_lvgl_adapter, plus a
// 90-degree rotation done in the flush path (the panel can't rotate itself).

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DISPLAY_ROT_NONE = 0, DISPLAY_ROT_CCW = 1, DISPLAY_ROT_CW = 2 } display_rot_t;

esp_err_t display_start(display_rot_t rot);   // after board_init()

// LVGL is not thread-safe: hold this around every lv_* call from any task.
// timeout_ms < 0 waits forever. Recursive.
esp_err_t display_lock(int timeout_ms);
void      display_unlock(void);

void          display_set_rotation(display_rot_t rot);   // caller holds the lock
display_rot_t display_get_rotation(void);

// "none" | "ccw" | "cw", as saved in NVS and typed at the console
int         display_rot_from_name(const char *name);     // -1 if not one of those
const char *display_rot_name(display_rot_t rot);

#ifdef __cplusplus
}
#endif
