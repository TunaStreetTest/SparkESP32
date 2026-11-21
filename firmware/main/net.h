#pragma once
// Wi-Fi transport. Configured over the USB console and kept in NVS:
//   ssids : comma-separated, tried in order (rotate after 3 failed joins)
//   pass  : shared WPA2/WPA3 passphrase (8-63 chars; empty = open network)
//   url   : the agent's endpoint, e.g. http://spark-ab12.local:8787/stat
#include <stdbool.h>
#include "esp_err.h"
#include "link.h"
#include "status.h"

#ifdef __cplusplus
extern "C" {
#endif

void      net_start(void);          // after settings_init(); starts the supervisor/poll task

bool   net_wifi_configured(void);   // at least one SSID
bool   net_wifi_is_connected(void);
bool   net_url_configured(void);
link_t net_link(void);              // a snapshot of the link's state machine
feed_t net_feed(void);              // how the last /stat poll went

// Console setters: validate, persist, apply. Return ESP_ERR_INVALID_ARG on bad input.
esp_err_t net_set_ssids(const char *csv);
esp_err_t net_set_pass(const char *pass);
esp_err_t net_set_url(const char *url);
esp_err_t net_clear(void);

void      net_show(void);           // NET-BEGIN..NET-END; never prints the password
esp_err_t net_scan_print(void);     // SCAN-BEGIN..SCAN-END

#ifdef __cplusplus
}
#endif
