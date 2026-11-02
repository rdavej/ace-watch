/*
 * Ace Watch — DevX Pulse face
 * Board: Waveshare ESP32-C6-Touch-AMOLED-2.06 (410 x 502 AMOLED)
 *
 * - Time + date (from Wi-Fi time sync; falls back to the build time)
 * - Battery % from the AXP2101 power chip
 * - Steps today + raise to wake (motion.c)
 * - Screen turns off after 20 s, tap or raise to wake,
 *   lower the wrist or cover with your palm to turn it off
 * - Hold the screen 3 s to open Wi-Fi setup
 * - Updates itself over Wi-Fi (see net.c)
 */
#include <stdio.h>
#include <stdarg.h>
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
#include "diag.h"
#include "workout.h"

LV_FONT_DECLARE(font_time_86);
LV_IMAGE_DECLARE(img_devx_logo_big);
LV_IMAGE_DECLARE(img_devx_logo_small);

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
static i2c_master_dev_handle_t s_tp = NULL;   /* FT3168 touch chip, read for palm cover */
#define TP_ADDR 0x38

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
    /* Touch chip: our own handle just to read how many fingers / palm */
    i2c_device_config_t tcfg = cfg;
    tcfg.device_address = TP_ADDR;
    if (i2c_master_bus_add_device(bsp_i2c_get_handle(), &tcfg, &s_tp) != ESP_OK) {
        ESP_LOGW(TAG, "Touch chip not readable, palm cover disabled");
        s_tp = NULL;
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
static esp_pm_lock_handle_t s_pm_screen = NULL; /* full CPU speed while the screen is on */

/* Fixed text buffers: labels point at these instead of allocating memory
   on every update (keeps RAM free for Wi-Fi / secure connections) */
static char s_time_buf[8], s_date_buf[24], s_bat_buf[40], s_steps_buf[16];
static char s_charge_buf[8], s_foot_buf[48];

static void label_printf(lv_obj_t *l, char *buf, size_t n, const char *fmt, ...)
{
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (lv_label_get_text(l) == buf && strcmp(tmp, buf) == 0) return; /* unchanged: no redraw */
    strlcpy(buf, tmp, n);
    lv_label_set_text_static(l, buf);
}
static bool s_wake_by_raise = false;
static bool s_wake_quiet = false;   /* woken by the watch itself (rest over): don't learn the angle */

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
    label_printf(s_time, s_time_buf, sizeof(s_time_buf), "%d:%02d", hour12, t.tm_min);

    static const char *wd[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    static const char *mo[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    label_printf(s_date, s_date_buf, sizeof(s_date_buf), "%s  %d %s", wd[t.tm_wday], t.tm_mday, mo[t.tm_mon]);
}

static void update_battery(void)
{
    bool charging;
    int pct = battery_percent(&charging);
    if (pct < 0) {
        label_printf(s_bat_label, s_bat_buf, sizeof(s_bat_buf), "%s", LV_SYMBOL_BATTERY_EMPTY " --");
        return;
    }
    const char *icon = charging ? LV_SYMBOL_CHARGE :
                       pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                       pct > 55 ? LV_SYMBOL_BATTERY_3 :
                       pct > 30 ? LV_SYMBOL_BATTERY_2 :
                       pct > 10 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    if (!charging && pct < 5) {
        label_printf(s_bat_label, s_bat_buf, sizeof(s_bat_buf), LV_SYMBOL_WARNING " %d%%  Charge me", pct);
    } else {
        label_printf(s_bat_label, s_bat_buf, sizeof(s_bat_buf), "%s %d%%", icon, pct);
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
        label_printf(s_steps, s_steps_buf, sizeof(s_steps_buf), "%lu,%03lu", (unsigned long)(n / 1000), (unsigned long)(n % 1000));
    } else {
        label_printf(s_steps, s_steps_buf, sizeof(s_steps_buf), "%lu", (unsigned long)n);
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
    label_printf(s_charge_pct, s_charge_buf, sizeof(s_charge_buf), "%d%%", s_charge_pct_val);

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
    /* Logs only when plugged in: with no laptop listening, log output can stall */
    static int s_log_on = -1;
    if ((int)plugged != s_log_on) {
        s_log_on = plugged;
        esp_log_level_set("*", plugged ? ESP_LOG_INFO : ESP_LOG_NONE);
    }
    if (plugged && !s_was_plugged) show_charge_screen(true);
    if (!plugged && s_was_plugged) show_charge_screen(false);
    s_was_plugged = plugged;

    if (s_toast && s_toast_until && (int32_t)(lv_tick_get() - s_toast_until) > 0) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until = 0;
    }
}

/* ---------- Palm cover ----------
   FT3168 registers: 0x01 gesture, 0x02 number of touch points (low 4 bits),
   then point 1 data. A palm shows up as 2+ points (or an invalid count). */
static uint8_t s_tp_last[8];

static int palm_check(void)
{
    if (!s_tp) return 0;
    uint8_t reg = 0x01, d[8] = {0};
    if (i2c_master_transmit_receive(s_tp, &reg, 1, d, sizeof(d), 20) != ESP_OK) return -1;
    if (memcmp(d, s_tp_last, sizeof(d)) != 0) {
        memcpy(s_tp_last, d, sizeof(d));
        /* Shown in the laptop monitor so we can tune palm detection */
        ESP_LOGI(TAG, "touch: gest %02x pts %02x | %02x %02x %02x %02x %02x %02x",
                 d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    }
    /* What the chip showed us (v0.3.4 log):
       finger:  reg01 = 01, points = 1, weight 00
       palm:    reg01 = 01 but points = 0 (the chip hides big touches),
                or a point with a heavy weight (c1)
       nothing: reg01 = 00, points = 0 */
    int gest = d[0];
    int pts = d[1] & 0x0F;
    int weight = d[6];
    if (pts == 0) return gest ? 2 : 0;
    if (pts == 1) return (weight >= 0x60 && weight != 0xFF) ? 2 : 1;
    return 2;                        /* 2+ points or invalid count: palm */
}

/* ---------- Screen on / off ---------- */
#define PALM_SAMPLES     3      /* x 250 ms = palm held ~0.75 s */
#define RAISE_MIN_ON_MS  2000   /* raise-woken screen stays on at least this long */

static uint32_t s_block_until = 0;  /* touches/raises before this tick can't wake the screen */
static bool s_palm_down = false;     /* palm is covering the screen right now */
static int s_palm_n = 0;
static int s_clear_n = 0;
static uint32_t s_on_since = 0;
static bool s_on_by_raise = false;   /* screen is on because the wrist was raised */

static void screen_off(void)
{
    if (!s_screen_on) return;
    bsp_display_brightness_set(0);
    lv_display_enable_invalidation(NULL, false); /* stop redrawing while dark */
    if (s_pm_screen) esp_pm_lock_release(s_pm_screen); /* now allowed to slow down / nap */
    s_screen_on = false;
    s_on_by_raise = false;
}

static void screen_on(void)
{
    if (s_screen_on) return;
    if (s_pm_screen) esp_pm_lock_acquire(s_pm_screen); /* full speed while visible */
    lv_display_enable_invalidation(NULL, true);
    update_clock();
    update_battery();
    update_steps();
    lv_obj_invalidate(lv_screen_active());        /* redraws on the next frame */
    bsp_display_brightness_set(SCREEN_BRIGHTNESS);
    s_screen_on = true;
    s_on_since = lv_tick_get();
    s_on_by_raise = s_wake_by_raise;
    /* Woken by a tap: learn how the wrist is held when looking at the watch */
    if (!s_wake_by_raise && !s_wake_quiet && !s_toast_until &&
        lv_obj_has_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN)) g_learn_view = true;
    s_wake_by_raise = false;
    s_wake_quiet = false;
}

/* Called from the motion task: light the screen straight away instead of
   waiting for the next screen-timer tick (that wait made raise-to-wake feel slow) */
void motion_on_raise(void)
{
    if (s_screen_on || s_palm_down) return;
    if (!bsp_display_lock(100)) return;   /* the power timer will catch it instead */
    if (!s_screen_on && !s_palm_down) {
        s_wake_by_raise = true;
        lv_display_trigger_activity(NULL);
        screen_on();
    }
    bsp_display_unlock();
}

static void power_timer_cb(lv_timer_t *t)
{
    (void)t;
    diag_ui_alive();   /* tell the freeze guard the screen task is running */
    uint32_t now = lv_tick_get();
    bool keep_on = g_net_state == NET_SETUP || g_net_state == NET_UPDATING;

    /* Palm: check while the screen is on, and while covered (to see it lift) */
    if (s_screen_on || s_palm_down) {
        int p = palm_check();
        if (p == 2) {
            s_clear_n = 0;
            if (!s_palm_down && ++s_palm_n >= PALM_SAMPLES && !keep_on) {
                s_palm_down = true;
                ESP_LOGI(TAG, "Palm cover: screen off");
                screen_off();
            }
        } else if (p == 0) {
            s_palm_n = 0;
            if (s_palm_down && ++s_clear_n >= 2) {
                s_palm_down = false;            /* palm lifted */
                s_block_until = now + 300;      /* the lift itself doesn't wake it */
            }
        } else if (p == 1) {
            s_palm_n = 0;
            if (s_palm_down) {
                /* a normal finger tap after the palm: palm is gone, let this tap wake it */
                s_palm_down = false;
                s_clear_n = 0;
                s_block_until = now - 1000;
            }
        }
    }
    if (s_palm_down) s_block_until = now + 300;

    /* Rest over: show the workout screen and light up */
    if (workout_tick()) {
        workout_show(true);
        if (!s_screen_on && !s_palm_down) {
            s_wake_quiet = true;
            lv_display_trigger_activity(NULL);
        } else if (s_screen_on) {
            lv_display_trigger_activity(NULL);   /* keep it on a full 20 s */
        }
    }

    /* Wrist raised: only matters when the screen is dark */
    if (g_raise_to_wake) {
        g_raise_to_wake = false;
        if (!s_screen_on && !s_palm_down) {
            s_wake_by_raise = true;
            lv_display_trigger_activity(NULL);
        }
    }

    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (keep_on) idle = 0;
    uint32_t last_act = now - idle;
    bool fresh = (int32_t)(last_act - s_block_until) > 0;  /* activity after the block */

    if (s_screen_on) {
        /* A touch after a raise-wake: treat it like a normal tap-wake from now on */
        if (s_on_by_raise && (int32_t)(last_act - s_on_since) > 300) s_on_by_raise = false;

        if (idle > SCREEN_TIMEOUT_MS) {
            screen_off();
        } else if (s_on_by_raise && g_wrist_down && !keep_on &&
                   now - s_on_since > RAISE_MIN_ON_MS) {
            ESP_LOGI(TAG, "Wrist lowered: screen off");
            screen_off();
            s_block_until = now;
        }
    } else if (idle < SCREEN_TIMEOUT_MS && (fresh || keep_on)) {
        screen_on();
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

static uint32_t s_boot_msg_until = 0;
static const char *s_shown_phase = NULL;

static void update_status(void)
{
    net_state_t st = g_net_state;
    const char *phase = (const char *)g_net_phase;
    bool boot_msg = g_diag_boot_msg[0] && (int32_t)(lv_tick_get() - s_boot_msg_until) < 0;
    if (boot_msg) {
        show_setup_panel(st == NET_SETUP);
        label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "%s", g_diag_boot_msg);
        s_shown_state = (net_state_t)-1;  /* redraw normal status afterwards */
        return;
    }
    if (st == s_shown_state && phase == s_shown_phase &&
        strcmp(s_shown_sync, g_net_last_sync) == 0) return;
    s_shown_state = st;
    s_shown_phase = phase;
    strlcpy(s_shown_sync, g_net_last_sync, sizeof(s_shown_sync));

    show_setup_panel(st == NET_SETUP);

    const char *ver = esp_app_get_description()->version;
    switch (st) {
    case NET_NO_WIFI:
        label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "%s", "Hold screen 3s for Wi-Fi setup");
        break;
    case NET_CONNECTING:
    case NET_SYNCING:
        label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "v%s  " LV_SYMBOL_WIFI " %s...", ver, phase);
        break;
    case NET_UPDATING:
        label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "%s", LV_SYMBOL_DOWNLOAD " Updating... keep watch on");
        break;
    case NET_OFFLINE:
        label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "v%s  offline", ver);
        break;
    case NET_OK:
        if (g_net_last_sync[0]) {
            label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "v%s  " LV_SYMBOL_WIFI " %s", ver, g_net_last_sync);
        } else {
            label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "v%s  " LV_SYMBOL_WIFI, ver);
        }
        break;
    default:
        label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "ACE WATCH  v%s", ver);
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
    if (s_palm_down || !s_screen_on || s_palm_n > 0) return; /* a palm, not a press */
    if (workout_visible()) return;
    net_request_setup();
}

/* Swipe left: workout screen. Swipe right: watch face. */
static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in || !s_screen_on) return;
    lv_dir_t d = lv_indev_get_gesture_dir(in);
    if (d == LV_DIR_LEFT && !workout_visible()) workout_show(true);
    else if (d == LV_DIR_RIGHT && workout_visible()) workout_show(false);
}

/* ---------- Startup: DevX logo + loading bar ---------- */
static lv_obj_t *s_splash;

static void splash_bar_cb(void *bar, int32_t v)
{
    lv_bar_set_value((lv_obj_t *)bar, v, LV_ANIM_OFF);
}

static void splash_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_splash) {
        lv_obj_delete_async(s_splash);   /* frees its memory; the face is underneath */
        s_splash = NULL;
    }
}

static void build_splash(void)
{
    s_splash = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_splash);
    lv_obj_set_size(s_splash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_splash, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);

    lv_obj_t *logo = lv_image_create(s_splash);
    lv_image_set_src(logo, &img_devx_logo_big);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, -30);

    lv_obj_t *bar = lv_bar_create(s_splash);
    lv_obj_set_size(bar, 150, 6);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(bar, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_BLUE), LV_PART_INDICATOR);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 110);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_duration(&a, 2200);
    lv_anim_set_delay(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_exec_cb(&a, splash_bar_cb);
    lv_anim_set_completed_cb(&a, splash_done_cb);
    lv_anim_start(&a);
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

    /* Header: DevX logo + DEVX */
    lv_obj_t *hdr = lv_obj_create(scr);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t *logo = lv_image_create(hdr);
    lv_image_set_src(logo, &img_devx_logo_small);
    lv_obj_t *brand = make_label(hdr, &lv_font_montserrat_20, COL_BLUE);
    lv_label_set_text(brand, "DEVX");
    lv_obj_set_style_text_letter_space(brand, 4, 0);

    /* Battery (small, under the header) */
    s_bat_label = make_label(scr, &lv_font_montserrat_16, COL_GREY);
    label_printf(s_bat_label, s_bat_buf, sizeof(s_bat_buf), "%s", "");
    lv_obj_align(s_bat_label, LV_ALIGN_TOP_MID, 0, 72);

    /* Big time */
    /* Big clock font (86 px), drawn directly: no extra RAM needed */
    s_time = make_label(scr, &font_time_86, COL_WHITE);
    label_printf(s_time, s_time_buf, sizeof(s_time_buf), "%s", "--:--");
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, -62);

    /* Date */
    s_date = make_label(scr, &lv_font_montserrat_24, COL_BLUE);
    lv_obj_set_style_text_letter_space(s_date, 3, 0);
    label_printf(s_date, s_date_buf, sizeof(s_date_buf), "%s", "");
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
    label_printf(s_steps, s_steps_buf, sizeof(s_steps_buf), "%s", "0");
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

    /* Workout screen (swipe left) */
    workout_build(scr);
    lv_obj_add_event_cb(scr, gesture_cb, LV_EVENT_GESTURE, NULL);

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
    label_printf(s_charge_pct, s_charge_buf, sizeof(s_charge_buf), "%s", "");
    lv_obj_align(s_charge_pct, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *cl = make_label(s_charge_panel, &lv_font_montserrat_20, COL_BLUE);
    lv_label_set_text(cl, LV_SYMBOL_CHARGE "  CHARGING");
    lv_obj_set_style_text_letter_space(cl, 4, 0);
    lv_obj_align(cl, LV_ALIGN_CENTER, 0, 130);
    lv_obj_add_flag(s_charge_panel, LV_OBJ_FLAG_HIDDEN);

    /* Footer */
    s_foot = make_label(scr, &lv_font_montserrat_16, COL_GREY);
    label_printf(s_foot, s_foot_buf, sizeof(s_foot_buf), "%s", "");
    lv_obj_set_style_text_letter_space(s_foot, 1, 0);
    lv_obj_align(s_foot, LV_ALIGN_BOTTOM_MID, 0, -44);
    lv_obj_move_to_index(s_foot, 0);   /* behind the workout screen */

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
    diag_boot();   /* black box: why did we restart last time? */

    /* Power saving: slow the CPU down and nap between jobs when idle */
    esp_pm_config_t pm = {
        .max_freq_mhz = 160,
        .min_freq_mhz = 40,
        .light_sleep_enable = true,
    };
    if (esp_pm_configure(&pm) != ESP_OK) {
        ESP_LOGW(TAG, "Power management not available");
    } else if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "screen", &s_pm_screen) == ESP_OK) {
        esp_pm_lock_acquire(s_pm_screen); /* screen starts on */
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    set_time_from_build();

    bsp_display_start();
    pmu_init();
    motion_start();

    diag_phase("UI");
    bsp_display_lock(0);
    build_ui();
    /* Logo on a normal start; skip it after a crash/freeze so the watch is back fast */
    if (!g_diag_boot_msg[0]) build_splash();
    s_boot_msg_until = lv_tick_get() + 60000;   /* 1 min, so it can be seen */
    lv_refr_now(NULL);   /* draw the full first frame before Wi-Fi starts */
    bsp_display_unlock();

    bsp_display_brightness_set(SCREEN_BRIGHTNESS);

    /* We got this far, so this firmware works: keep it (no rollback) */
    esp_ota_mark_app_valid_cancel_rollback();

    diag_start_guard();   /* restart by itself if the screen ever freezes */
    diag_phase("idle");

    net_start();
}
