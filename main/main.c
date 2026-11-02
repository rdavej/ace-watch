/*
 * Ace Watch v0.1 — DevX Pulse face
 * Board: Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 x 502 AMOLED)
 *
 * - Time + date (set from the laptop's clock when built; Wi-Fi time comes later)
 * - Battery % from the AXP2101 power chip
 * - Screen turns off after 10 s, tap to wake
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

static const char *TAG = "ace_watch";

/* Electric look */
#define COL_BG      0x000000
#define COL_BLUE    0x3FB8FF
#define COL_YELLOW  0xFFD23F
#define COL_GREY    0x8C9AB8
#define COL_TRACK   0x13203D
#define COL_WHITE   0xE9EEF8

#define SCREEN_TIMEOUT_MS  10000
#define SCREEN_BRIGHTNESS  80

/* ---------- AXP2101 power chip (battery) ---------- */
#define AXP2101_ADDR        0x34
#define AXP_REG_STATUS1     0x00
#define AXP_REG_STATUS2     0x01
#define AXP_REG_GAUGE_CTRL  0x18
#define AXP_REG_BAT_PERCENT 0xA4

static i2c_master_dev_handle_t s_pmu = NULL;

static esp_err_t pmu_read(uint8_t reg, uint8_t *val)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_pmu, &reg, 1, val, 1, 50);
}

static esp_err_t pmu_write(uint8_t reg, uint8_t val)
{
    if (!s_pmu) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_pmu, buf, 2, 50);
}

static void pmu_init(void)
{
    if (bsp_i2c_init() != ESP_OK) {
        ESP_LOGW(TAG, "I2C not available, battery disabled");
        return;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &cfg, &s_pmu) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not found, battery disabled");
        s_pmu = NULL;
        return;
    }
    /* Make sure the fuel gauge is on (bit 3) */
    uint8_t v;
    if (pmu_read(AXP_REG_GAUGE_CTRL, &v) == ESP_OK) {
        pmu_write(AXP_REG_GAUGE_CTRL, v | 0x08);
    }
}

/* Returns 0-100, or -1 if unknown. *charging set when charging. */
static int battery_percent(bool *charging)
{
    uint8_t st1 = 0, st2 = 0, pct = 0;
    *charging = false;
    if (pmu_read(AXP_REG_STATUS1, &st1) != ESP_OK) return -1;
    if (!(st1 & 0x08)) return -1;                 /* no battery detected */
    if (pmu_read(AXP_REG_STATUS2, &st2) == ESP_OK) {
        *charging = ((st2 >> 5) & 0x03) == 0x01;  /* 01 = charging */
    }
    if (pmu_read(AXP_REG_BAT_PERCENT, &pct) != ESP_OK) return -1;
    if (pct > 100) pct = 100;
    return pct;
}

/* ---------- Clock ---------- */
static void set_time_from_build(void)
{
    setenv("TZ", "MYT-8", 1);   /* Malaysia, UTC+8 */
    tzset();

    time_t now;
    time(&now);
    if (now > 1700000000) return; /* already set */

    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = {0};
    int d = 1, y = 2026, h = 0, mi = 0, s = 0;
    sscanf(__DATE__, "%3s %d %d", mon, &d, &y);
    sscanf(__TIME__, "%d:%d:%d", &h, &mi, &s);
    const char *p = strstr(months, mon);

    struct tm t = {0};
    t.tm_year = y - 1900;
    t.tm_mon = p ? (int)(p - months) / 3 : 0;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;

    /* +60 s roughly covers build + flash time */
    struct timeval tv = { .tv_sec = mktime(&t) + 60, .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

/* ---------- UI ---------- */
static lv_obj_t *s_time;
static lv_obj_t *s_date;
static lv_obj_t *s_bat_label;
static lv_obj_t *s_bat_bar;
static bool s_screen_on = true;

static void update_clock(void)
{
    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);

    int hour12 = t.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    lv_label_set_text_fmt(s_time, "%d:%02d", hour12, t.tm_min);

    static const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *mo[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    lv_label_set_text_fmt(s_date, "%s  %d %s", wd[t.tm_wday], t.tm_mday, mo[t.tm_mon]);
}

static void update_battery(void)
{
    bool charging;
    int pct = battery_percent(&charging);
    if (pct < 0) {
        lv_label_set_text(s_bat_label, "Battery --");
        lv_bar_set_value(s_bat_bar, 0, LV_ANIM_OFF);
        return;
    }
    lv_label_set_text_fmt(s_bat_label, "%s %d%%", charging ? LV_SYMBOL_CHARGE : LV_SYMBOL_BATTERY_FULL, pct);
    lv_bar_set_value(s_bat_bar, pct, LV_ANIM_OFF);
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_clock();
}

static void battery_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_battery();
}

/* Screen off after inactivity, any touch wakes it */
static void power_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (idle > SCREEN_TIMEOUT_MS && s_screen_on) {
        bsp_display_brightness_set(0);
        s_screen_on = false;
    } else if (idle < SCREEN_TIMEOUT_MS && !s_screen_on) {
        bsp_display_brightness_set(SCREEN_BRIGHTNESS);
        s_screen_on = true;
        update_clock();
        update_battery();
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header: DEVX */
    lv_obj_t *brand = make_label(scr, &lv_font_montserrat_20, COL_BLUE);
    lv_label_set_text(brand, LV_SYMBOL_CHARGE " DEVX");
    lv_obj_set_style_text_letter_space(brand, 4, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 48);

    /* Big time */
    s_time = make_label(scr, &lv_font_montserrat_48, COL_WHITE);
    lv_label_set_text(s_time, "--:--");
    lv_obj_set_style_transform_scale(s_time, 460, 0);   /* ~1.8x */
    lv_obj_set_style_transform_pivot_x(s_time, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(s_time, lv_pct(50), 0);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -70);

    /* Date */
    s_date = make_label(scr, &lv_font_montserrat_24, COL_BLUE);
    lv_obj_set_style_text_letter_space(s_date, 3, 0);
    lv_label_set_text(s_date, "");
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 20);

    /* Battery */
    s_bat_label = make_label(scr, &lv_font_montserrat_24, COL_YELLOW);
    lv_label_set_text(s_bat_label, "");
    lv_obj_align(s_bat_label, LV_ALIGN_CENTER, 0, 92);

    s_bat_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bat_bar, 260, 10);
    lv_bar_set_range(s_bat_bar, 0, 100);
    lv_obj_set_style_bg_color(s_bat_bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bat_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bat_bar, lv_color_hex(COL_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bat_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bat_bar, 5, LV_PART_INDICATOR);
    lv_obj_align(s_bat_bar, LV_ALIGN_CENTER, 0, 128);

    /* Footer */
    lv_obj_t *foot = make_label(scr, &lv_font_montserrat_16, COL_GREY);
    lv_label_set_text(foot, "ACE WATCH  v0.1");
    lv_obj_set_style_text_letter_space(foot, 2, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -44);

    update_clock();
    update_battery();

    lv_timer_create(clock_timer_cb, 1000, NULL);
    lv_timer_create(battery_timer_cb, 5000, NULL);
    lv_timer_create(power_timer_cb, 250, NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ace Watch v0.1 starting");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    set_time_from_build();

    bsp_display_start();
    pmu_init();

    bsp_display_lock(0);
    build_ui();
    bsp_display_unlock();

    bsp_display_brightness_set(SCREEN_BRIGHTNESS);
}
