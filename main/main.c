/*
 * Atlas Helmet HUD (v0.9.0) — HUD only
 * Board: Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 x 502 AMOLED)
 *
 * The watch firmware (watch face, steps, DevX alerts, switches...) was removed: the board
 * now starts straight into the HUD. Saved Wi-Fi networks and Wi-Fi updates are kept.
 * The old watch code is in the git history (v0.8.0 and earlier).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "diag.h"
#include "hud.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "hud";

/* ---------- AXP2101 power chip (battery) ---------- */
#define AXP2101_ADDR        0x34
#define AXP_REG_STATUS1     0x00
#define AXP_REG_GAUGE_CTRL  0x18
#define AXP_REG_BAT_PERCENT 0xA4

static i2c_master_dev_handle_t s_pmu;

static esp_err_t pmu_read(uint8_t reg, uint8_t *val)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_pmu, &reg, 1, val, 1, 50);
}

static void pmu_init(void)
{
    if (bsp_i2c_init() != ESP_OK) return;
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_pmu) != ESP_OK) { s_pmu = NULL; return; }
    uint8_t v;
    if (pmu_read(AXP_REG_GAUGE_CTRL, &v) == ESP_OK) {   /* fuel gauge on */
        uint8_t buf[2] = { AXP_REG_GAUGE_CTRL, (uint8_t)(v | 0x08) };
        i2c_master_transmit(s_pmu, buf, 2, 50);
    }
}

/* 0-100, or -1 if unknown (shown on the dash) */
int watch_battery(void)
{
    uint8_t st1 = 0, pct = 0;
    if (pmu_read(AXP_REG_STATUS1, &st1) != ESP_OK || !(st1 & 0x08)) return -1;
    if (pmu_read(AXP_REG_BAT_PERCENT, &pct) != ESP_OK) return -1;
    return pct > 100 ? 100 : pct;
}

/* ---------- Clock: build time until Wi-Fi sets it ---------- */
static void set_time_from_build(void)
{
    setenv("TZ", "MYT-8", 1);   /* Malaysia, UTC+8 */
    tzset();
    if (time(NULL) > 1700000000) return;
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int d = 1, y = 2026, h = 0, mi = 0, s = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
    const char *p = strstr(months, mon);
    struct tm t = { .tm_year = y - 1900, .tm_mon = p ? (int)(p - months) / 3 : 0, .tm_mday = d,
                    .tm_hour = h, .tm_min = mi, .tm_sec = s, .tm_isdst = -1 };
    struct timeval tv = { .tv_sec = mktime(&t) + 60, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

static void alive_cb(lv_timer_t *t)
{
    (void)t;
    diag_ui_alive();   /* tell the freeze guard the screen task is running */
}

void app_main(void)
{
    ESP_LOGI(TAG, "Atlas HUD v%s starting", esp_app_get_description()->version);
    diag_boot();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    set_time_from_build();
    settings_load();

    bsp_display_start();
    pmu_init();

    diag_phase("UI");
    bsp_display_lock(0);
    lv_indev_t *in = bsp_display_get_input_dev();
    if (in) lv_indev_set_long_press_time(in, 3000);   /* hold 3 s for set-up */
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_COVER, 0);
    hud_open();
    lv_timer_create(alive_cb, 1000, NULL);
    bsp_display_unlock();

    /* this firmware started fine: keep it (no rollback) */
    esp_ota_mark_app_valid_cancel_rollback();

    diag_start_guard();
    diag_phase("idle");
    net_start();
}
