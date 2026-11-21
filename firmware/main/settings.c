#include "settings.h"

#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#define NS "sparkesp32"

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t settings_get(const char *key, char *buf, size_t len, const char *def)
{
    strlcpy(buf, def ? def : "", len);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_ERR_NOT_FOUND;
    size_t need = 0;
    err = nvs_get_str(h, key, NULL, &need);
    if (err == ESP_OK && need > len) err = ESP_ERR_INVALID_SIZE;   // never truncate silently
    if (err == ESP_OK) err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) strlcpy(buf, def ? def : "", len);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t settings_set(const char *key, const char *val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = val ? nvs_set_str(h, key, val) : nvs_erase_key(h, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;          // erasing an absent key
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_erase_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(h);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}
