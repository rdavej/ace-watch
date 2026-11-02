/*
 * Ace Watch networking
 *  - Wi-Fi setup hotspot "Ace-Watch-Setup" with a small web page (captive portal)
 *  - Remembers up to 5 networks (home, office, gym...)
 *  - Every 30 min: join the strongest known network, sync time, check for an update, then turn Wi-Fi off
 *  - Updates (OTA) come from the latest GitHub release of rdavej/ace-watch
 */
#include "net.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "lwip/sockets.h"

static const char *TAG = "net";

#define MAX_NETS          5
#define MAX_SCAN          15
#define SETUP_SSID        "Ace-Watch-Setup"
#define SYNC_INTERVAL_MS  (30 * 60 * 1000)
#define SETUP_TIMEOUT_MS  (5 * 60 * 1000)
#define FW_BASE_URL       "https://github.com/rdavej/ace-watch/releases/latest/download/"

typedef struct {
    char ssid[33];
    char pass[65];
} wifi_net_t;

volatile net_state_t g_net_state = NET_IDLE;
char g_net_last_sync[16] = "";

static wifi_net_t s_nets[MAX_NETS];
static int s_net_count;

static char s_scan[MAX_SCAN][33];
static int s_scan_n;

static EventGroupHandle_t s_ev;
#define EV_GOT_IP     BIT0
#define EV_FAIL       BIT1
#define EV_SETUP_REQ  BIT2
#define EV_SAVED      BIT3

static volatile bool s_connecting;
static volatile bool s_dns_run;

/* ---------------- Saved networks (NVS) ---------------- */

static void nets_count(void)
{
    s_net_count = 0;
    for (int i = 0; i < MAX_NETS; i++) {
        if (s_nets[i].ssid[0]) s_net_count++;
    }
}

static void nets_load(void)
{
    memset(s_nets, 0, sizeof(s_nets));
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
        size_t sz = sizeof(s_nets);
        if (nvs_get_blob(h, "nets", s_nets, &sz) != ESP_OK) {
            memset(s_nets, 0, sizeof(s_nets));
        }
        nvs_close(h);
    }
    for (int i = 0; i < MAX_NETS; i++) {
        s_nets[i].ssid[32] = 0;
        s_nets[i].pass[64] = 0;
    }
    nets_count();
}

static void nets_save(void)
{
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "nets", s_nets, sizeof(s_nets));
    nvs_commit(h);
    nvs_close(h);
}

static void nets_add(const char *ssid, const char *pass)
{
    int slot = -1;
    for (int i = 0; i < MAX_NETS; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < MAX_NETS; i++) {
            if (!s_nets[i].ssid[0]) { slot = i; break; }
        }
    }
    if (slot < 0) { /* full: drop the oldest */
        memmove(&s_nets[0], &s_nets[1], sizeof(wifi_net_t) * (MAX_NETS - 1));
        slot = MAX_NETS - 1;
    }
    memset(&s_nets[slot], 0, sizeof(wifi_net_t));
    strlcpy(s_nets[slot].ssid, ssid, sizeof(s_nets[slot].ssid));
    strlcpy(s_nets[slot].pass, pass, sizeof(s_nets[slot].pass));
    nets_save();
    nets_count();
    ESP_LOGI(TAG, "Saved network '%s' (%d stored)", ssid, s_net_count);
}

/* ---------------- Wi-Fi ---------------- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
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
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
}

/* Blocking scan, fills s_scan with unique network names */
static void wifi_scan(wifi_ap_record_t *recs, uint16_t *n)
{
    wifi_scan_config_t sc = {0};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) { *n = 0; return; }
    esp_wifi_scan_get_ap_records(n, recs);

    s_scan_n = 0;
    for (int j = 0; j < *n && s_scan_n < MAX_SCAN; j++) {
        const char *name = (const char *)recs[j].ssid;
        if (!name[0]) continue;
        bool dup = false;
        for (int k = 0; k < s_scan_n; k++) {
            if (strcmp(s_scan[k], name) == 0) { dup = true; break; }
        }
        if (!dup) strlcpy(s_scan[s_scan_n++], name, sizeof(s_scan[0]));
    }
}

static bool wifi_connect(void)
{
    if (!s_net_count) return false;

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (esp_wifi_start() != ESP_OK) return false;

    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (!recs) { esp_wifi_stop(); return false; }
    wifi_scan(recs, &n);

    int best = -1;
    int best_rssi = -1000;
    for (int j = 0; j < n; j++) {
        for (int i = 0; i < MAX_NETS; i++) {
            if (s_nets[i].ssid[0] && strcmp((const char *)recs[j].ssid, s_nets[i].ssid) == 0 &&
                recs[j].rssi > best_rssi) {
                best = i;
                best_rssi = recs[j].rssi;
            }
        }
    }
    free(recs);

    if (best < 0) {
        ESP_LOGI(TAG, "No saved network nearby");
        esp_wifi_stop();
        return false;
    }

    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, s_nets[best].ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_nets[best].pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_STA, &wc);

    ESP_LOGI(TAG, "Connecting to '%s' (%d dBm)", s_nets[best].ssid, best_rssi);
    xEventGroupClearBits(s_ev, EV_GOT_IP | EV_FAIL);
    s_connecting = true;
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP | EV_FAIL, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    s_connecting = false;

    if (b & EV_GOT_IP) return true;

    ESP_LOGW(TAG, "Could not connect");
    esp_wifi_stop();
    return false;
}

static void wifi_off(void)
{
    s_connecting = false;
    esp_wifi_disconnect();
    esp_wifi_stop();
}

/* ---------------- Time ---------------- */

static void time_sync(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&cfg) != ESP_OK) return;
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) == ESP_OK) {
        time_t now = time(NULL);
        struct tm t;
        localtime_r(&now, &t);
        int h = t.tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(g_net_last_sync, sizeof(g_net_last_sync), "%d:%02d", h, t.tm_min);
        ESP_LOGI(TAG, "Time synced");
    } else {
        ESP_LOGW(TAG, "Time sync timed out");
    }
    esp_netif_sntp_deinit();
}

/* ---------------- Updates (OTA) ---------------- */

typedef struct {
    char buf[40];
    int len;
} rxbuf_t;

static esp_err_t http_ev(esp_http_client_event_t *e)
{
    rxbuf_t *r = (rxbuf_t *)e->user_data;
    if (!r) return ESP_OK;
    if (e->event_id == HTTP_EVENT_ON_CONNECTED) {
        r->len = 0; /* new connection (e.g. after a redirect) */
    } else if (e->event_id == HTTP_EVENT_ON_DATA) {
        int n = e->data_len;
        if (r->len + n > (int)sizeof(r->buf) - 1) n = (int)sizeof(r->buf) - 1 - r->len;
        if (n > 0) {
            memcpy(r->buf + r->len, e->data, n);
            r->len += n;
        }
    }
    return ESP_OK;
}

static void trim(char *s)
{
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    memmove(s, p, strlen(p) + 1);
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
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
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Update check failed (err %d, HTTP %d)", err, status);
        return;
    }
    rx.buf[rx.len] = 0;
    trim(rx.buf);

    const char *mine = esp_app_get_description()->version;
    ESP_LOGI(TAG, "Firmware: this %s, latest %s", mine, rx.buf);
    if (!rx.buf[0] || strcmp(rx.buf, mine) == 0) return;

    g_net_state = NET_UPDATING;
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
        ESP_LOGI(TAG, "Update installed, restarting");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    ESP_LOGE(TAG, "Update failed");
}

/* ---------------- Setup hotspot (captive portal) ---------------- */

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { vTaskDelete(NULL); return; }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t buf[512];
    while (s_dns_run) {
        struct sockaddr_in src;
        socklen_t sl = sizeof(src);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&src, &sl);
        if (n < 12) continue;
        if (((buf[4] << 8) | buf[5]) < 1) continue;

        /* Skip the question name */
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5; /* zero byte + type + class */
        if (p > n) continue;
        uint16_t qtype = (buf[p - 4] << 8) | buf[p - 3];

        /* Answer every name with the watch's own address */
        buf[2] = 0x81; buf[3] = 0x80;
        buf[4] = 0; buf[5] = 1;
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1) {
            const uint8_t ans[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
            memcpy(buf + p, ans, sizeof(ans));
            p += sizeof(ans);
        }
        sendto(sock, buf, p, 0, (struct sockaddr *)&src, sl);
    }
    close(sock);
    vTaskDelete(NULL);
}

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') {
            *o++ = ' ';
            s++;
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(h, NULL, 16);
            s += 3;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static void form_get(const char *body, const char *key, char *out, size_t outlen)
{
    size_t kl = strlen(key);
    const char *p = body;
    out[0] = 0;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            const char *e = strchr(p, '&');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n >= outlen) n = outlen - 1;
            memcpy(out, p, n);
            out[n] = 0;
            url_decode(out);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static void send_escaped(httpd_req_t *r, const char *s)
{
    char buf[200];
    size_t o = 0;
    for (; *s && o < sizeof(buf) - 8; s++) {
        switch (*s) {
        case '<': memcpy(buf + o, "&lt;", 4); o += 4; break;
        case '>': memcpy(buf + o, "&gt;", 4); o += 4; break;
        case '&': memcpy(buf + o, "&amp;", 5); o += 5; break;
        case '"': memcpy(buf + o, "&quot;", 6); o += 6; break;
        case '\'': memcpy(buf + o, "&#39;", 5); o += 5; break;
        default: buf[o++] = *s;
        }
    }
    buf[o] = 0;
    httpd_resp_sendstr_chunk(r, buf);
}

static const char PAGE_HEAD[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Ace Watch</title><style>"
    "body{background:#000;color:#E9EEF8;font-family:-apple-system,Roboto,sans-serif;padding:24px;max-width:420px;margin:auto}"
    "h1{color:#3FB8FF;font-size:22px;letter-spacing:2px}"
    "label{display:block;margin:18px 0 6px;color:#8C9AB8}"
    "select,input{width:100%;padding:12px;font-size:16px;border-radius:12px;border:1px solid #1C2A4A;"
    "background:#0D1528;color:#E9EEF8;box-sizing:border-box}"
    "button{margin-top:26px;width:100%;padding:14px;font-size:17px;font-weight:700;border:0;border-radius:28px;"
    "background:#3FB8FF;color:#00122B}p{color:#8C9AB8;line-height:1.5}b{color:#FFD23F}"
    "</style></head><body><h1>&#9889; DEVX &middot; ACE WATCH</h1>";

static esp_err_t root_get(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    httpd_resp_sendstr_chunk(r, PAGE_HEAD);
    httpd_resp_sendstr_chunk(r,
        "<p>Add a Wi-Fi network. The watch remembers up to 5 (home, office, gym) "
        "and joins whichever is nearby.</p>"
        "<form method=post action=/save><label>Wi-Fi network</label><select name=s1>");
    for (int i = 0; i < s_scan_n; i++) {
        httpd_resp_sendstr_chunk(r, "<option>");
        send_escaped(r, s_scan[i]);
        httpd_resp_sendstr_chunk(r, "</option>");
    }
    httpd_resp_sendstr_chunk(r,
        "<option value=''>Other (type below)</option></select>"
        "<label>Or type the network name</label>"
        "<input name=s2 autocapitalize=none autocomplete=off autocorrect=off>"
        "<label>Password</label><input name=p type=password>"
        "<button>Save</button></form>");
    if (s_net_count) {
        httpd_resp_sendstr_chunk(r, "<p>Already saved: ");
        bool first = true;
        for (int i = 0; i < MAX_NETS; i++) {
            if (!s_nets[i].ssid[0]) continue;
            if (!first) httpd_resp_sendstr_chunk(r, ", ");
            send_escaped(r, s_nets[i].ssid);
            first = false;
        }
        httpd_resp_sendstr_chunk(r, "</p>");
    }
    httpd_resp_sendstr_chunk(r, "</body></html>");
    httpd_resp_sendstr_chunk(r, NULL);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *r)
{
    char body[512];
    int len = r->content_len;
    if (len <= 0 || len >= (int)sizeof(body)) {
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Bad request");
    }
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        got += n;
    }
    body[got] = 0;

    char s1[100], s2[100], pw[200];
    form_get(body, "s1", s1, sizeof(s1));
    form_get(body, "s2", s2, sizeof(s2));
    form_get(body, "p", pw, sizeof(pw));
    const char *ssid = s2[0] ? s2 : s1;

    if (!ssid[0] || strlen(ssid) > 32 || strlen(pw) > 64) {
        httpd_resp_set_status(r, "302 Found");
        httpd_resp_set_hdr(r, "Location", "/");
        return httpd_resp_send(r, NULL, 0);
    }

    nets_add(ssid, pw);

    httpd_resp_set_type(r, "text/html");
    httpd_resp_sendstr_chunk(r, PAGE_HEAD);
    httpd_resp_sendstr_chunk(r, "<p><b>Saved: ");
    send_escaped(r, ssid);
    httpd_resp_sendstr_chunk(r,
        "</b></p><p>The watch will now connect and set its time. "
        "You can close this page and rejoin your normal Wi-Fi.</p></body></html>");
    httpd_resp_sendstr_chunk(r, NULL);

    xEventGroupSetBits(s_ev, EV_SAVED);
    return ESP_OK;
}

static esp_err_t redirect_404(httpd_req_t *r, httpd_err_code_t err)
{
    httpd_resp_set_status(r, "302 Found");
    httpd_resp_set_hdr(r, "Location", "http://192.168.4.1/");
    httpd_resp_send(r, "Redirect", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static void run_setup(void)
{
    ESP_LOGI(TAG, "Starting setup hotspot");
    g_net_state = NET_SETUP;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, SETUP_SSID, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(SETUP_SSID);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    uint16_t n = 20;
    wifi_ap_record_t *recs = calloc(n, sizeof(wifi_ap_record_t));
    if (recs) {
        wifi_scan(recs, &n);
        free(recs);
    }

    httpd_handle_t server = NULL;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = 5;
    hc.lru_purge_enable = true;
    if (httpd_start(&server, &hc) == ESP_OK) {
        const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_get };
        const httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &save);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_404);
    }

    s_dns_run = true;
    xTaskCreate(dns_task, "dns", 3072, NULL, 5, NULL);

    xEventGroupClearBits(s_ev, EV_SAVED);
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_SAVED, pdTRUE, pdFALSE, pdMS_TO_TICKS(SETUP_TIMEOUT_MS));
    if (b & EV_SAVED) vTaskDelay(pdMS_TO_TICKS(2500)); /* let the "Saved" page reach the phone */

    s_dns_run = false;
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (server) httpd_stop(server);
    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "Setup hotspot closed");
}

/* ---------------- Main loop ---------------- */

static void net_task(void *arg)
{
    nets_load();
    bool first = true;

    for (;;) {
        if (!s_net_count) {
            g_net_state = NET_NO_WIFI;
            if (first) {
                first = false;
                run_setup(); /* first boot: open setup automatically */
            } else {
                xEventGroupWaitBits(s_ev, EV_SETUP_REQ, pdTRUE, pdFALSE, portMAX_DELAY);
                run_setup();
            }
            continue;
        }
        first = false;

        g_net_state = NET_CONNECTING;
        if (wifi_connect()) {
            g_net_state = NET_SYNCING;
            time_sync();
            ota_check();
            wifi_off();
            g_net_state = NET_OK;
        } else {
            g_net_state = NET_OFFLINE;
        }

        EventBits_t b = xEventGroupWaitBits(s_ev, EV_SETUP_REQ, pdTRUE, pdFALSE, pdMS_TO_TICKS(SYNC_INTERVAL_MS));
        if (b & EV_SETUP_REQ) run_setup();
    }
}

void net_start(void)
{
    s_ev = xEventGroupCreate();
    wifi_init();
    xTaskCreate(net_task, "net", 10240, NULL, 4, NULL);
}

void net_request_setup(void)
{
    if (s_ev && g_net_state != NET_SETUP && g_net_state != NET_UPDATING) {
        xEventGroupSetBits(s_ev, EV_SETUP_REQ);
    }
}
