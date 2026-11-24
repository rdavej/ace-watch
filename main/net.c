/*
 * Helmet HUD networking (v0.9.0, HUD only)
 *  - Uses the Wi-Fi networks saved by the Ace Watch firmware (same storage)
 *  - At start-up and every 30 min: join the strongest saved network, sync the clock,
 *    install an update if there's a newer one, then turn Wi-Fi off
 *  - Updates come from the latest GitHub release of rdavej/ace-watch
 */
#include "net.h"
#include "diag.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "net";

#define MAX_NETS      5
#define SYNC_EVERY_MS (30 * 60 * 1000)
#define FW_BASE_URL   "https://github.com/rdavej/ace-watch/releases/latest/download/"

typedef struct { char ssid[33]; char pass[65]; } wifi_net_t;

static wifi_net_t s_nets[MAX_NETS];
static int s_net_count;
static EventGroupHandle_t s_ev;
static volatile bool s_connecting;
static char s_status[48] = "Wi-Fi: starting";

#define EV_GOT_IP BIT0
#define EV_FAIL   BIT1
#define EV_SYNC   BIT2

const char *net_status(void) { return s_status; }

static void status(const char *s)
{
    strlcpy(s_status, s, sizeof(s_status));
    ESP_LOGI(TAG, "%s", s);
}

static void nets_load(void)
{
    memset(s_nets, 0, sizeof(s_nets));
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_nets);
        if (nvs_get_blob(h, "nets", s_nets, &sz) != ESP_OK) memset(s_nets, 0, sizeof(s_nets));
        nvs_close(h);
    }
    s_net_count = 0;
    for (int i = 0; i < MAX_NETS; i++) {
        s_nets[i].ssid[32] = 0;
        s_nets[i].pass[64] = 0;
        if (s_nets[i].ssid[0]) s_net_count++;
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_connecting) xEventGroupSetBits(s_ev, EV_FAIL);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_ev, EV_GOT_IP);
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(err);
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
}

static bool wifi_connect(void)
{
    if (!s_net_count) return false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return false;
    esp_wifi_set_max_tx_power(60);

    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_stop(); return false; }
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) n = 0;
    else esp_wifi_scan_get_ap_records(&n, recs);

    /* saved networks in range, strongest first */
    int cand[MAX_NETS], rssi[MAX_NETS], nc = 0;
    for (int i = 0; i < MAX_NETS; i++) {
        if (!s_nets[i].ssid[0]) continue;
        int best = -1000;
        for (int j = 0; j < n; j++)
            if (strcmp((const char *)recs[j].ssid, s_nets[i].ssid) == 0 && recs[j].rssi > best) best = recs[j].rssi;
        if (best == -1000) continue;
        int k = nc++;
        while (k > 0 && rssi[k - 1] < best) { cand[k] = cand[k - 1]; rssi[k] = rssi[k - 1]; k--; }
        cand[k] = i;
        rssi[k] = best;
    }
    free(recs);

    for (int c = 0; c < nc && c < 3; c++) {
        const wifi_net_t *w = &s_nets[cand[c]];
        wifi_config_t wc = {0};
        strlcpy((char *)wc.sta.ssid, w->ssid, sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, w->pass, sizeof(wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &wc);
        xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
        s_connecting = true;
        esp_wifi_connect();
        EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        s_connecting = false;
        if (b & EV_GOT_IP) return true;
        esp_wifi_disconnect();
    }
    esp_wifi_stop();
    return false;
}

static void wifi_off(void)
{
    s_connecting = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
}

static void time_sync(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) return;
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
    esp_netif_sntp_deinit();
}

/* ---- updates ---- */
typedef struct { char buf[40]; int len; } rxbuf_t;

static esp_err_t http_ev(esp_http_client_event_t *e)
{
    rxbuf_t *r = (rxbuf_t *)e->user_data;
    if (!r) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_CONNECTED) r->len = 0;
    else if (e->event_id == HTTP_EVENT_ON_DATA) {
        int n = e->data_len;
        if (r->len + n > (int)sizeof(r->buf) - 1) n = (int)sizeof(r->buf) - 1 - r->len;
        if (n > 0) { memcpy(r->buf + r->len, e->data, n); r->len += n; }
    }
    return ESP_OK;
}

static int version_cmp(const char *a, const char *b)
{
    int va[3] = {0}, vb[3] = {0};
    sscanf(a, "%d.%d.%d", &va[0], &va[1], &va[2]);
    sscanf(b, "%d.%d.%d", &vb[0], &vb[1], &vb[2]);
    for (int i = 0; i < 3; i++) if (va[i] != vb[i]) return va[i] - vb[i];
    return 0;
}

static void ota_check(void)
{
    rxbuf_t rx = {0};
    esp_http_client_config_t vc = {
        .url = FW_BASE_URL "version.txt",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .max_redirection_count = 5,
        .event_handler = http_ev,
        .user_data = &rx,
    };
    esp_http_client_handle_t c = esp_http_client_init(&vc);
    if (!c) return;
    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (err != ESP_OK || code != 200) { status("Update check failed"); return; }
    rx.buf[rx.len] = 0;
    char *p = rx.buf;
    while (*p && isspace((unsigned char)*p)) p++;
    for (int l = strlen(p); l && isspace((unsigned char)p[l - 1]); ) p[--l] = 0;

    const char *mine = esp_app_get_description()->version;
    if (!p[0] || version_cmp(p, mine) <= 0) { status("Up to date"); return; }

    char b[48];
    snprintf(b, sizeof(b), "Updating to v%s...", p);
    status(b);
    esp_http_client_config_t bc = {
        .url = FW_BASE_URL "ace_watch.bin",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t oc = { .http_config = &bc };
    if (esp_https_ota(&oc) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    status("Update didn't install");
}

static void net_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));   /* let the start-up play first */
    wifi_init();
    nets_load();
    for (;;) {
        if (!s_net_count) {
            status("No saved Wi-Fi");
        } else {
            status("Wi-Fi: connecting");
            diag_phase("Wi-Fi");
            if (wifi_connect()) {
                status("Wi-Fi: checking");
                diag_phase("Update");
                time_sync();
                ota_check();
            } else {
                status("Wi-Fi: none nearby");
            }
            wifi_off();
            diag_phase("idle");
        }
        EventBits_t b = xEventGroupWaitBits(s_ev, EV_SYNC, pdTRUE, pdFALSE, pdMS_TO_TICKS(SYNC_EVERY_MS));
        (void)b;
    }
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    xTaskCreate(net_task, "net", 8192, NULL, 3, NULL);
}

void net_sync_now(void)
{
    if (s_ev) xEventGroupSetBits(s_ev, EV_SYNC);
}
