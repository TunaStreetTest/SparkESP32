// Host-test stand-in for ESP-IDF's esp_timer.h: a settable clock.
#pragma once
#include <stdint.h>

extern int64_t g_fake_us;
static inline int64_t esp_timer_get_time(void) { return g_fake_us; }
