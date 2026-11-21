#pragma once
// Persistent string settings in NVS (namespace "sparkesp32"). Plaintext at
// rest — fine for a display on your desk, documented in SECURITY.md.
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t settings_init(void);   // nvs_flash_init, erase+retry on layout change

// Copies the value into buf (always NUL-terminated); a missing key, or a value
// that doesn't fit in len, gives def instead (never a truncated value).
void      settings_get(const char *key, char *buf, size_t len, const char *def);
esp_err_t settings_set(const char *key, const char *val);   // val NULL erases

#ifdef __cplusplus
}
#endif
