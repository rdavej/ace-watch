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
#include "esp_pm.h"
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
#define COL_RED     0xFF5A5A

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

/* True when USB power is plugged in (VBUS good, status 1 bit 5) */
static bool usb_plugged(void)
{
    uint8_t st1 = 0;
    if (pmu_read(AXP_REG_STATUS1, &st1) != ESP_OK) return false;
    return (st1 & 0x20) != 0;
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
static bool s_steps_ready = false; /* false until the first update after boot */
static lv_obj_t *s_foot;
static lv_obj_t *s_setup_panel;
static bool s_screen_on = true;
static bool s_wake_by_raise = false;

/* goal */
static bool s_goal_done = false;
static lv_obj_t *s_toast;
static uint32_t s_toast_until = 0;

/* charging screen */
static lv_obj_t *s_charge_panel;
static lv_obj_t *s_charge_pct;
static lv_obj_t *s_plates[2][3];   /* [side][0 = inner .. 2 = outer] */
static int s_plate_frame = 0;
static int s_charge_pct_val = 0;
static uint32_t s_charge_until = 0;
static bool s_was_plugged = false;
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
    if (!charging && pct < 5) {
        lv_label_set_text_fmt(s_bat_label, LV_SYMBOL_WARNING " %d%%  Charge me", pct);
    } else {
        lv_label_set_text_fmt(s_bat_label, "%s %d%%", icon, pct);
    }
    lv_obj_set_style_text_color(s_bat_label, lv_color_hex((!charging && pct < 15) ? COL_RED : COL_GREY), 0);
    s_charge_pct_val = pct;
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

    bool first = !s_steps_ready;
    s_steps_ready = true;
    bool reached = n >= STEP_GOAL;
    lv_obj_set_style_bg_color(s_steps_bar, lv_color_hex(reached ? COL_YELLOW : COL_BLUE), LV_PART_INDICATOR);
    if (reached && !s_goal_done) {
        s_goal_done = true;
        if (!first && n < STEP_GOAL + 50) { /* just crossed it, not a reboot */
            lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
            s_toast_until = lv_tick_get() + 5000;
            lv_display_trigger_activity(NULL);                 /* light up to celebrate */
        }
    } else if (!reached) {
        s_goal_done = false;                                   /* new day */
    }
}

static void clock_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_clock();
    update_steps();
}

/* Barbell: plates load one by one; how many depends on the battery level */
static void charge_anim_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_charge_panel || lv_obj_has_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN)) return;

    int target = s_charge_pct_val >= 100 ? 3 : (s_charge_pct_val + 33) / 34; /* 1..3 plates a side */
    if (target < 1) target = 1;
    int shown = s_charge_pct_val >= 100 ? 3 : (s_plate_frame % (target + 2)); /* 0..target, short pause */
    if (shown > target) shown = target;
    for (int side = 0; side < 2; side++) {
        for (int i = 0; i < 3; i++) {
            if (i < shown) lv_obj_remove_flag(s_plates[side][i], LV_OBJ_FLAG_HIDDEN);
            else lv_obj_add_flag(s_plates[side][i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    s_plate_frame++;
    lv_label_set_text_fmt(s_charge_pct, "%d%%", s_charge_pct_val);

    if ((int32_t)(lv_tick_get() - s_charge_until) > 0) {
        lv_obj_add_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_charge_screen(bool show)
{
    if (show) {
        s_plate_frame = 0;
        s_charge_until = lv_tick_get() + 8000;
        lv_obj_remove_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN);
        lv_display_trigger_activity(NULL);
    } else {
        lv_obj_add_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void charge_panel_tap_cb(lv_event_t *e)
{
    (void)e;
    show_charge_screen(false);
}

static void battery_timer_cb(lv_timer_t *t)
{
    (void)t;
    update_battery();

    bool plugged = usb_plugged();
    if (plugged && !s_was_plugged) show_charge_screen(true);
    if (!plugged && s_was_plugged) show_charge_screen(false);
    s_was_plugged = plugged;

    if (s_toast && s_toast_until && (int32_t)(lv_tick_get() - s_toast_until) > 0) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
}

/* Screen off after inactivity, any touch wakes it */
static void power_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (g_raise_to_wake) {
        g_raise_to_wake = false;
        if (!s_screen_on) s_wake_by_raise = true;
        lv_display_trigger_activity(NULL); /* wrist raised: count as a touch */
    }
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (g_net_state == NET_SETUP || g_net_state == NET_UPDATING) {
        idle = 0; /* keep the screen on while setting up / updating */
    }
    if (idle > SCREEN_TIMEOUT_MS && s_screen_on) {
        bsp_display_brightness_set(0);
        lv_display_enable_invalidation(NULL, false); /* stop redrawing while dark */
        s_screen_on = false;
    } else if (idle < SCREEN_TIMEOUT_MS && !s_screen_on) {
        lv_display_enable_invalidation(NULL, true);
        update_clock();
        update_battery();
        update_steps();
        lv_obj_invalidate(lv_screen_active());        /* redraws on the next frame */
        bsp_display_brightness_set(SCREEN_BRIGHTNESS);
        s_screen_on = true;
        /* Woken by a tap: learn how the wrist is held when looking at the watch */
        if (!s_wake_by_raise && !s_toast_until &&
            lv_obj_has_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN)) g_learn_view = true;
        s_wake_by_raise = false;
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

    /* Goal reached toast */
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_toast);
    lv_obj_set_size(s_toast, 300, 64);
    lv_obj_set_style_radius(s_toast, 32, 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(COL_YELLOW), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, -150);
    lv_obj_t *tl = make_label(s_toast, &lv_font_montserrat_24, 0x00122B);
    lv_label_set_text(tl, LV_SYMBOL_OK "  GOAL REACHED!");
    lv_obj_center(tl);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* Charging screen: barbell loading plates */
    s_charge_panel = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_charge_panel);
    lv_obj_set_size(s_charge_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_charge_panel, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_charge_panel, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_charge_panel, charge_panel_tap_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *bar = lv_obj_create(s_charge_panel);           /* the bar */
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 330, 12);
    lv_obj_set_style_radius(bar, 6, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x5A6680), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, -50);

    for (int side = 0; side < 2; side++) {
        lv_obj_t *collar = lv_obj_create(s_charge_panel);   /* collars */
        lv_obj_remove_style_all(collar);
        lv_obj_set_size(collar, 10, 34);
        lv_obj_set_style_radius(collar, 3, 0);
        lv_obj_set_style_bg_color(collar, lv_color_hex(0x8C9AB8), 0);
        lv_obj_set_style_bg_opa(collar, LV_OPA_COVER, 0);
        lv_obj_align(collar, LV_ALIGN_CENTER, side ? 66 : -66, -50);

        static const int heights[3] = {130, 110, 88};
        for (int i = 0; i < 3; i++) {                        /* plates, inner to outer */
            lv_obj_t *pl = lv_obj_create(s_charge_panel);
            lv_obj_remove_style_all(pl);
            lv_obj_set_size(pl, 18, heights[i]);
            lv_obj_set_style_radius(pl, 5, 0);
            lv_obj_set_style_bg_color(pl, lv_color_hex(i == 0 ? COL_BLUE : i == 1 ? 0x2E8FD0 : 0x1F6BA8), 0);
            lv_obj_set_style_bg_opa(pl, LV_OPA_COVER, 0);
            int x = 86 + i * 24;
            lv_obj_align(pl, LV_ALIGN_CENTER, side ? x : -x, -50);
            lv_obj_add_flag(pl, LV_OBJ_FLAG_HIDDEN);
            s_plates[side][i] = pl;
        }
    }

    s_charge_pct = make_label(s_charge_panel, &lv_font_montserrat_48, COL_YELLOW);
    lv_label_set_text(s_charge_pct, "");
    lv_obj_align(s_charge_pct, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *cl = make_label(s_charge_panel, &lv_font_montserrat_20, COL_BLUE);
    lv_label_set_text(cl, LV_SYMBOL_CHARGE "  CHARGING");
    lv_obj_set_style_text_letter_space(cl, 4, 0);
    lv_obj_align(cl, LV_ALIGN_CENTER, 0, 130);
    lv_obj_add_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN);

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
    lv_timer_create(battery_timer_cb, 2000, NULL);
    lv_timer_create(charge_anim_cb, 350, NULL);
    lv_timer_create(power_timer_cb, 250, NULL);
    lv_timer_create(status_timer_cb, 500, NULL);
    lv_timer_create(heap_timer_cb, 10000, NULL);
    update_status();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Ace Watch v%s starting", esp_app_get_description()->version);

    /* Power saving: slow the CPU down and nap between jobs when idle */
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "Power management not available");

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
