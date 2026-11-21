// net — Wi-Fi transport for SparkESP32.
//
// One task supervises the link and polls the agent:
//   * joins the configured SSIDs in order (rotating after 3 failed attempts),
//     pacing attempts with exponential backoff — connects are never issued
//     from the Wi-Fi event handler, so a dead AP can't cause a reconnect storm
//   * once a second, GETs the agent's /stat (skipped while a USB sample is
//     fresh — USB always wins) and feeds the shared parser
// The Wi-Fi driver is told to keep its config in RAM only, so the passphrase
// lives in exactly one place: our NVS namespace (see SECURITY.md).

#include "net.h"
#include "settings.h"
#include "spark_data.h"
#include "display.h"
#include "ui.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "net";

#define MAX_SSIDS        4
#define SSID_MAX         32
#define PASS_MAX         63
#define URL_MAX          127
#define FAILS_TO_ROTATE  3
#define BACKOFF_MIN_MS   2000
#define BACKOFF_MAX_MS   30000
#define ATTEMPT_TIMEOUT_MS 20000     // no event at all for this long -> retry
#define USB_FRESH_MS     3000        // a USB sample younger than this suppresses polling
#define HTTP_TIMEOUT_MS  2500
#define EV_CONNECTED     BIT0

static EventGroupHandle_t s_ev;
static SemaphoreHandle_t s_mtx;      // guards the config + supervisor state below
static esp_netif_t *s_netif;

static char s_ssids[MAX_SSIDS][SSID_MAX + 1];
static int  s_nssids, s_cur, s_fails;
static char s_pass[PASS_MAX + 1];
static char s_url[URL_MAX + 1];

static bool s_connecting;
static uint32_t s_attempt_ms, s_next_try_ms, s_backoff_ms = BACKOFF_MIN_MS;
static volatile bool s_paused;       // set while scanning
static char s_last_err[64];
static unsigned s_poll_ok, s_poll_err;

#define LOCK()   xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static void set_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    LOCK();
    vsnprintf(s_last_err, sizeof(s_last_err), fmt, ap);
    UNLOCK();
    va_end(ap);
}

// ---------------------------------------------------------------- config

static int split_ssids(const char *csv, char out[MAX_SSIDS][SSID_MAX + 1])
{
    int n = 0;
    const char *p = csv;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == 0 || len > SSID_MAX || n == MAX_SSIDS) return -1;
        memcpy(out[n], p, len);
        out[n][len] = 0;
        n++;
        if (!comma) break;
        p = comma + 1;
        if (!*p) return -1;                    // trailing comma
    }
    return n;
}

// The one place the saved config becomes the RAM copy the supervisor uses.
static void load_cfg(void)
{
    char csv[MAX_SSIDS * (SSID_MAX + 1) + 1];
    char tmp[MAX_SSIDS][SSID_MAX + 1] = {0};
    settings_get("ssids", csv, sizeof(csv), "");
    int n = csv[0] ? split_ssids(csv, tmp) : 0;
    LOCK();
    s_nssids = n > 0 ? n : 0;
    if (s_cur >= s_nssids) s_cur = 0;
    memcpy(s_ssids, tmp, sizeof(tmp));
    memset(s_pass, 0, sizeof(s_pass));           // don't leave an old passphrase's tail in RAM
    settings_get("pass", s_pass, sizeof(s_pass), "");
    settings_get("url", s_url, sizeof(s_url), "");
    UNLOCK();
}

bool net_wifi_configured(void) { return s_nssids > 0; }
bool net_url_configured(void) { return s_url[0] != 0; }

bool net_wifi_is_connected(void)
{
    return s_ev && (xEventGroupGetBits(s_ev) & EV_CONNECTED);
}

// Build the driver config for the current SSID. Caller holds s_mtx.
static void build_config(wifi_config_t *wc)
{
    memset(wc, 0, sizeof(*wc));
    strlcpy((char *)wc->sta.ssid, s_ssids[s_cur], sizeof(wc->sta.ssid));
    strlcpy((char *)wc->sta.password, s_pass, sizeof(wc->sta.password));
    // threshold is a floor: a WPA2 floor still joins WPA3 / transition APs
    wc->sta.threshold.authmode = s_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc->sta.pmf_cfg.capable = true;
}

// Drop the current link and let the supervisor start over with the new config.
static void restart_link(void)
{
    LOCK();
    s_cur = s_fails = 0;
    s_backoff_ms = BACKOFF_MIN_MS;
    s_next_try_ms = spark_now_ms();
    s_connecting = false;
    UNLOCK();
    esp_wifi_disconnect();
}

// Persist one setting, reload the RAM config, and (for link settings) rejoin.
static esp_err_t save(const char *key, const char *val, bool relink)
{
    esp_err_t err = settings_set(key, val);
    if (err != ESP_OK) return err;
    load_cfg();
    if (relink) restart_link();
    return ESP_OK;
}

esp_err_t net_set_ssids(const char *csv)
{
    char tmp[MAX_SSIDS][SSID_MAX + 1];
    if (split_ssids(csv, tmp) <= 0) return ESP_ERR_INVALID_ARG;
    return save("ssids", csv, true);
}

esp_err_t net_set_pass(const char *pass)
{
    size_t len = strlen(pass);
    if (len != 0 && (len < 8 || len > PASS_MAX)) return ESP_ERR_INVALID_ARG;   // WPA2/3 PSK rules
    return save("pass", pass, true);
}

esp_err_t net_set_url(const char *url)
{
    if (strncmp(url, "http://", 7) != 0 || strlen(url) > URL_MAX || !url[7])
        return ESP_ERR_INVALID_ARG;                // plain HTTP on the LAN only
    return save("url", url, false);
}

esp_err_t net_clear(void)
{
    esp_err_t err = settings_set("ssids", NULL);
    if (err == ESP_OK) err = settings_set("pass", NULL);
    if (err == ESP_OK) err = settings_set("url", NULL);
    load_cfg();                                    // whatever NVS now holds
    restart_link();
    return err;
}

// ---------------------------------------------------------------- wifi events

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_CONNECTED);
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        LOCK();
        if (s_connecting) {                        // a failed attempt: keep its backoff deadline
            s_fails++;
            snprintf(s_last_err, sizeof(s_last_err), "join '%s' reason %d",
                     s_nssids ? s_ssids[s_cur] : "", d ? d->reason : -1);
        } else {
            // A link that was up dropped (or we dropped it): retry now. This also
            // keeps the deadline recent; one set at the last join could be over
            // 2^31 ms old after weeks connected and wrap to "in the future".
            s_next_try_ms = spark_now_ms();
        }
        s_connecting = false;
        UNLOCK();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        LOCK();
        s_connecting = false;
        s_fails = 0;
        s_backoff_ms = BACKOFF_MIN_MS;
        ESP_LOGI(TAG, "joined '%s', ip " IPSTR, s_ssids[s_cur], IP2STR(&e->ip_info.ip));
        UNLOCK();
        xEventGroupSetBits(s_ev, EV_CONNECTED);
    }
}

// Called once a second from the net task. Decides under the lock, then talks
// to the driver outside it (the event handler needs the same lock).
static void supervise_link(void)
{
    if (s_paused || net_wifi_is_connected()) return;
    uint32_t now = spark_now_ms();
    wifi_config_t wc;
    LOCK();
    if (!s_nssids ||
        (s_connecting && (uint32_t)(now - s_attempt_ms) < ATTEMPT_TIMEOUT_MS) ||
        (int32_t)(now - s_next_try_ms) < 0) {
        UNLOCK();
        return;
    }
    if (s_fails >= FAILS_TO_ROTATE && s_nssids > 1) {
        s_cur = (s_cur + 1) % s_nssids;
        s_fails = 0;
        ESP_LOGW(TAG, "trying next ssid '%s'", s_ssids[s_cur]);
    }
    build_config(&wc);
    s_connecting = true;
    s_attempt_ms = now;
    s_next_try_ms = now + s_backoff_ms;
    s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
    UNLOCK();

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err == ESP_OK) err = esp_wifi_connect();
    memset(&wc, 0, sizeof(wc));                    // don't leave the passphrase on the stack
    if (err != ESP_OK) {
        LOCK();
        s_connecting = false;
        UNLOCK();
        set_err("connect %s", esp_err_to_name(err));
    }
}

// ---------------------------------------------------------------- poll

// GET the agent's /stat into buf. Returns body length, or -1 (sets s_last_err).
static int http_get(const char *url, char *buf, int cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 512,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { set_err("http init"); return -1; }
    int n = -1;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        int64_t clen = esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (st != 200) {
            set_err("http %d", st);   // 503 = agent stale
        } else if (clen <= 0 || clen >= cap) {
            set_err("bad length %lld", (long long)clen);
        } else {
            int tot = esp_http_client_read_response(c, buf, (int)clen);
            if (tot < 0) tot = 0;
            buf[tot] = 0;
            if (tot == clen) n = tot;
            else set_err("short body %d/%lld", tot, (long long)clen);
        }
    } else {
        set_err("connect %s", esp_err_to_name(err));
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return n;
}

static void poll_once(void)
{
    static char body[512];
    char url[URL_MAX + 1];
    LOCK();
    strlcpy(url, s_url, sizeof(url));
    UNLOCK();
    if (!url[0] || !net_wifi_is_connected()) return;

    if (display_lock(500) != ESP_OK) return;
    bool usb_fresh = g_spark.valid && g_spark.source == SRC_USB && spark_age_ms() < USB_FRESH_MS;
    display_unlock();
    if (usb_fresh) return;                         // USB wins

    if (http_get(url, body, sizeof(body)) <= 0) { s_poll_err++; return; }

    char *argv[10];
    int argc = 0;
    char *tok = NULL;
    for (char *t = strtok_r(body, " \t\r\n", &tok); t && argc < 10; t = strtok_r(NULL, " \t\r\n", &tok))
        argv[argc++] = t;
    // too many fields is usually an agent speaking an older protocol: say so plainly
    const char *why = "too many fields: update the agent";
    int rc = -1;
    if (argc < 10) {
        if (display_lock(1000) != ESP_OK) { s_poll_err++; return; }
        rc = spark_apply_args(argc, argv, SRC_WIFI, &why);
        if (rc == 0) ui_update();
        display_unlock();
    }
    if (rc == 0) {
        s_poll_ok++;
    } else {
        s_poll_err++;
        set_err("bad stat line (%s)", why);
    }
}

static void net_task(void *arg)
{
    while (1) {
        supervise_link();
        poll_once();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ---------------------------------------------------------------- start

static void purge_driver_copy(void)
{
    // Older firmware let the Wi-Fi driver persist SSID + passphrase in its own
    // NVS namespace. We use RAM storage now; wipe any copy left behind.
    nvs_handle_t h;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    s_mtx = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_netif, "sparkesp32");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    purge_driver_copy();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    load_cfg();
    ESP_ERROR_CHECK(esp_wifi_start());          // started even unconfigured so `wifi scan` works
    // Mains-powered display: modem power-save only adds RX latency spikes.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "wifi up: %d ssid(s), url %s", s_nssids, s_url[0] ? s_url : "(unset)");

    xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 4, NULL, 0);
}

// ---------------------------------------------------------------- console helpers

void net_show(void)
{
    char ip[16] = "-";
    int rssi = 0;
    if (net_wifi_is_connected()) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_netif, &info) == ESP_OK)
            snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    }
    LOCK();
    printf("NET-BEGIN\n");
    printf("configured=%d connected=%d cur='%s' ip=%s rssi=%d\n", s_nssids > 0,
           net_wifi_is_connected(), s_nssids ? s_ssids[s_cur] : "", ip, rssi);
    printf("ssids=");
    for (int i = 0; i < s_nssids; i++) printf("%s%s", i ? "," : "", s_ssids[i]);
    printf(" pass=%s url=%s\n", s_pass[0] ? "set" : "unset", s_url[0] ? s_url : "unset");
    printf("poll ok=%u err=%u last_err='%s'\n", s_poll_ok, s_poll_err, s_last_err);
    printf("NET-END\n");
    UNLOCK();
}

static const char *auth_name(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:          return "open";
    case WIFI_AUTH_WEP:           return "wep";
    case WIFI_AUTH_WPA_PSK:       return "wpa";
    case WIFI_AUTH_WPA2_PSK:      return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "wpa/wpa2";
    case WIFI_AUTH_WPA3_PSK:      return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "wpa2/wpa3";
    default:                      return "other";
    }
}

esp_err_t net_scan_print(void)
{
    s_paused = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    wifi_scan_config_t sc = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err == ESP_OK) {
        uint16_t n = 24;
        static wifi_ap_record_t recs[24];
        esp_wifi_scan_get_ap_records(&n, recs);
        printf("SCAN-BEGIN %d\n", n);
        for (int i = 0; i < n; i++) {
            char ssid[33];
            strlcpy(ssid, (const char *)recs[i].ssid, sizeof(ssid));
            for (char *c = ssid; *c; c++)          // printable only: no ANSI/marker injection
                if (!isprint((unsigned char)*c) || *c == '\'') *c = '?';
            printf("ap ssid='%s' chan=%d rssi=%d auth=%s\n", ssid, recs[i].primary,
                   recs[i].rssi, auth_name(recs[i].authmode));
        }
        printf("SCAN-END\n");
    } else {
        printf("ERR scan %s\nSCAN-END\n", esp_err_to_name(err));
    }
    restart_link();
    s_paused = false;
    return err;
}
