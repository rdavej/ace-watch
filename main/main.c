/*
 * Ace Watch — DevX Pulse face
 * Board: Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 x 502 AMOLED)
 *
 * - Time + date (from Wi-Fi time sync; falls back to the build time)
 * - Battery % from the AXP2101 power chip
 * - Steps today + raise to wake (motion.c)
 * - Screen turns off after 10 s, tap to wake
 * - Hold the screen 3 s to open Wi-Fi setup
 * - Updates itself over Wi-Fi (see net.c)
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
#include "esp_heap_caps.h"
#include "net.h"
#include "motion.h"

LV_FONT_DECLARE(font_time_86);

static const char *TAG = "ace_watch";

/* Electric look */
#define COL_BG      0x000000
#define COL_BLUE    0x3FB8FF
#define COL_YELLOW  0xFFD23F
#define COL_GREY    0x8C9AB8
#define COL_TRACK   0x13203D
#define COL_WHITE   0xE9EEF8

#define SCREEN_TIMEOUT_MS  20000
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
static lv_obj_t *s_steps;
static lv_obj_t *s_steps_bar;
static uint32_t s_shown_steps = UINT32_MAX;
static lv_obj_t *s_foot;
static lv_obj_t *s_setup_panel;
static bool s_screen_on = true;
static net_state_t s_shown_state = (net_state_t)-1;
static char s_shown_sync[16] = "";

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
        lv_label_set_text(s_bat_label, LV_SYMBOL_BATTERY_EMPTY " --");
        return;
    }
    const char *icon = charging ? LV_SYMBOL_CHARGE :
                       pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                       pct > 55 ? LV_SYMBOL_BATTERY_3 :
                       pct > 30 ? LV_SYMBOL_BATTERY_2 :
                       pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    lv_label_set_text_fmt(s_bat_label, "%s %d%%", icon, pct);
}

static void update_steps(void)
{
    uint32_t n = g_steps_today;
    if (n == s_shown_steps) return;
    s_shown_steps = n;
    if (n >= 1000) {
        lv_label_set_text_fmt(s_steps, "%lu,%03lu", (unsigned long)(n / 1000), (unsigned long)(n % 1000));
    } else {
        lv_label_set_text_fmt(s_steps, "%lu", (unsigned long)n);
    }
    lv_bar_set_value(s_steps_bar, n > STEP_GOAL ? STEP_GOAL : (int32_t)n, LV_ANIM_OFF);
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_clock();
    update_steps();
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
    if (g_raise_to_wake) {
        g_raise_to_wake = false;
        lv_display_trigger_activity(NULL); /* wrist raised: count as a touch */
    }
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (g_net_state == NET_SETUP || g_net_state == NET_UPDATING) {
        idle = 0; /* keep the screen on while setting up / updating */
    }
    if (idle > SCREEN_TIMEOUT_MS && s_screen_on) {
        bsp_display_brightness_set(0);
        s_screen_on = false;
    } else if (idle < SCREEN_TIMEOUT_MS && !s_screen_on) {
        bsp_display_brightness_set(SCREEN_BRIGHTNESS);
        s_screen_on = true;
        update_clock();
        update_battery();
        update_steps();
    }
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color);

static void show_setup_panel(bool show)
{
    if (show && !s_setup_panel) {
        s_setup_panel = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(s_setup_panel);
        lv_obj_set_size(s_setup_panel, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(s_setup_panel, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(s_setup_panel, LV_OPA_COVER, 0);

        lv_obj_t *t = make_label(s_setup_panel, &lv_font_montserrat_24, COL_BLUE);
        lv_label_set_text(t, LV_SYMBOL_WIFI "  WI-FI SETUP");
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 60);

        lv_obj_t *b = make_label(s_setup_panel, &lv_font_montserrat_20, COL_WHITE);
        lv_label_set_text(b,
            "1. On your phone, join\n    the Wi-Fi network\n\n"
            "2. A page opens by itself\n    (or go to 192.168.4.1)\n\n"
            "3. Pick your Wi-Fi,\n    type the password, Save");
        lv_obj_align(b, LV_ALIGN_CENTER, 0, 10);

        lv_obj_t *n = make_label(s_setup_panel, &lv_font_montserrat_24, COL_YELLOW);
        lv_label_set_text(n, "Ace-Watch-Setup");
        lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 110);

        lv_obj_t *f = make_label(s_setup_panel, &lv_font_montserrat_16, COL_GREY);
        lv_label_set_text(f, "Closes by itself after 5 min");
        lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, -44);
    } else if (!show && s_setup_panel) {
        lv_obj_delete(s_setup_panel);
        s_setup_panel = NULL;
    }
}

static void update_status(void)
{
    net_state_t st = g_net_state;
    if (st == s_shown_state && strcmp(s_shown_sync, g_net_last_sync) == 0) return;
    s_shown_state = st;
    strlcpy(s_shown_sync, g_net_last_sync, sizeof(s_shown_sync));

    show_setup_panel(st == NET_SETUP);

    const char *ver = esp_app_get_description()->version;
    switch (st) {
    case NET_NO_WIFI:
        lv_label_set_text(s_foot, "Hold screen 3s for Wi-Fi setup");
        break;
    case NET_CONNECTING:
    case NET_SYNCING:
        lv_label_set_text_fmt(s_foot, "v%s  " LV_SYMBOL_WIFI " syncing...", ver);
        break;
    case NET_UPDATING:
        lv_label_set_text(s_foot, LV_SYMBOL_DOWNLOAD " Updating... keep watch on");
        break;
    case NET_OFFLINE:
        lv_label_set_text_fmt(s_foot, "v%s  offline", ver);
        break;
    case NET_OK:
        if (g_net_last_sync[0]) {
            lv_label_set_text_fmt(s_foot, "v%s  " LV_SYMBOL_WIFI " %s", ver, g_net_last_sync);
        } else {
            lv_label_set_text_fmt(s_foot, "v%s  " LV_SYMBOL_WIFI, ver);
        }
        break;
    default:
        lv_label_set_text_fmt(s_foot, "ACE WATCH  v%s", ver);
        break;
    }
}

static void heap_timer_cb(lv_timer_t *t)
{
    (void)t;
    ESP_LOGI(TAG, "heap free %u, largest block %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void status_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_status();
}

static void long_press_cb(lv_event_t *e)
{
    (void)e;
    net_request_setup();
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
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 40);

    /* Battery (small, under the header) */
    s_bat_label = make_label(scr, &lv_font_montserrat_16, COL_GREY);
    lv_label_set_text(s_bat_label, "");
    lv_obj_align(s_bat_label, LV_ALIGN_TOP_MID, 0, 70);

    /* Big time */
    /* Big clock font (86 px), drawn directly: no extra RAM needed */
    s_time = make_label(scr, &font_time_86, COL_WHITE);
    lv_label_set_text(s_time, "--:--");
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -62);

    /* Date */
    s_date = make_label(scr, &lv_font_montserrat_24, COL_BLUE);
    lv_obj_set_style_text_letter_space(s_date, 3, 0);
    lv_label_set_text(s_date, "");
    lv_obj_align(s_date, LV_ALIGN_CENTER, 0, 18);

    /* Steps: "6,240 / 8,000 steps" + progress bar */
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 84);

    s_steps = make_label(row, &lv_font_montserrat_24, COL_YELLOW);
    lv_label_set_text(s_steps, "0");
    lv_obj_t *goal = make_label(row, &lv_font_montserrat_16, COL_GREY);
    lv_label_set_text(goal, "/ 8,000 steps");
    lv_obj_set_style_pad_bottom(goal, 2, 0);

    s_steps_bar = lv_bar_create(scr);
    lv_obj_set_size(s_steps_bar, 260, 10);
    lv_bar_set_range(s_steps_bar, 0, STEP_GOAL);
    lv_obj_set_style_bg_color(s_steps_bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_steps_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_steps_bar, lv_color_hex(COL_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_steps_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(s_steps_bar, 5, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_steps_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_steps_bar, LV_ALIGN_CENTER, 0, 120);

    /* Footer */
    s_foot = make_label(scr, &lv_font_montserrat_16, COL_GREY);
    lv_label_set_text(s_foot, "");
    lv_obj_set_style_text_letter_space(s_foot, 1, 0);
    lv_obj_align(s_foot, LV_ALIGN_BOTTOM_MID, 0, -44);

    /* Hold the screen 3 s to open Wi-Fi setup */
    lv_obj_add_event_cb(scr, long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) lv_indev_set_long_press_time(indev, 3000);

    update_clock();
    update_battery();
    update_steps();

    lv_timer_create(clock_timer_cb, 1000, NULL);
    lv_timer_create(battery_timer_cb, 5000, NULL);
    lv_timer_create(power_timer_cb, 250, NULL);
    lv_timer_create(status_timer_cb, 500, NULL);
    lv_timer_create(heap_timer_cb, 10000, NULL);
    update_status();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ace Watch v%s starting", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    set_time_from_build();

    bsp_display_start();
    pmu_init();
    motion_start();

    bsp_display_lock(0);
    build_ui();
    lv_refr_now(NULL);   /* draw the full first frame before Wi-Fi starts */
    bsp_display_unlock();

    bsp_display_brightness_set(SCREEN_BRIGHTNESS);

    /* We got this far, so this firmware works: keep it (no rollback) */
    esp_ota_mark_app_valid_cancel_rollback();

    net_start();
}
