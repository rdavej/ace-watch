/*
 * Stopwatch (for timing clients)
 *   START / STOP     the big right button
 *   LAP / RESET      the left button: LAP while running, RESET when stopped
 *   Laps             newest first, fastest lap in yellow (up to 30 kept)
 * Keeps running when you leave the screen or the screen goes dark;
 * the watch face shows it in the bottom row while it runs.
 */
#include "stopwatch.h"

#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "sound.h"

#define COL_BG      0x000000
#define COL_BLUE    0x3FB8FF
#define COL_YELLOW  0xFFD23F
#define COL_GREY    0x8C9AB8
#define COL_TRACK   0x13203D
#define COL_WHITE   0xE9EEF8
#define COL_DARK    0x00122B

#define MAX_LAPS 30

/* State (survives leaving the screen) */
static bool s_running;
static int64_t s_start_us;      /* when the current run started */
static int64_t s_acc_us;        /* time from earlier runs (before STOP) */
static int64_t s_lap_from_us;   /* total time at the last lap */
static int64_t s_laps[MAX_LAPS];
static int s_lap_n;

/* Screen (only while it's open) */
static lv_obj_t *s_main, *s_tenth, *s_left_btn, *s_left_lbl, *s_right_btn, *s_right_lbl, *s_list;
static lv_timer_t *s_timer;
static char s_main_buf[12], s_tenth_buf[4];
static int s_shown_laps = -1;

static int64_t total_us(void)
{
    return s_acc_us + (s_running ? esp_timer_get_time() - s_start_us : 0);
}

static void fmt(char *out, size_t n, int64_t us, bool tenths)
{
    int64_t ds = us / 100000;             /* tenths of a second */
    int t = (int)(ds % 10);
    int64_t s = ds / 10;
    if (s >= 3600) snprintf(out, n, "%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else snprintf(out, n, "%d:%02d", (int)(s / 60), (int)(s % 60));
    if (tenths) {
        size_t l = strlen(out);
        snprintf(out + l, n - l, ".%d", t);
    }
}

bool stopwatch_running(void) { return s_running; }

void stopwatch_text(char *out, size_t n) { fmt(out, n, total_us(), false); }

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void set_static(lv_obj_t *l, char *buf, size_t n, const char *txt)
{
    if (lv_label_get_text(l) == buf && strcmp(buf, txt) == 0) return;
    strlcpy(buf, txt, n);
    lv_label_set_text_static(l, buf);
}

static void style_buttons(void)
{
    if (!s_right_btn) return;
    lv_obj_set_style_bg_color(s_right_btn, lv_color_hex(s_running ? COL_YELLOW : COL_BLUE), 0);
    lv_label_set_text(s_right_lbl, s_running ? "STOP" : (total_us() ? "GO" : "START"));
    bool reset = !s_running && total_us();
    lv_label_set_text(s_left_lbl, reset ? "RESET" : "LAP");
    lv_obj_set_style_text_color(s_left_lbl, lv_color_hex(s_running || reset ? COL_WHITE : COL_TRACK), 0);
}

static void fill_laps(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    int best = -1;
    for (int i = 0; i < s_lap_n; i++) if (best < 0 || s_laps[i] < s_laps[best]) best = i;
    for (int i = s_lap_n - 1; i >= 0; i--) {             /* newest first */
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 30);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        uint32_t c = (i == best && s_lap_n > 1) ? COL_YELLOW : COL_WHITE;
        char t[24];
        snprintf(t, sizeof(t), "Lap %d", i + 1);
        lv_obj_t *a = label(row, &lv_font_montserrat_20, COL_GREY);
        lv_label_set_text(a, t);
        lv_obj_align(a, LV_ALIGN_LEFT_MID, 0, 0);
        fmt(t, sizeof(t), s_laps[i], true);
        lv_obj_t *b = label(row, &lv_font_montserrat_20, c);
        lv_label_set_text(b, t);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    s_shown_laps = s_lap_n;
}

static void update(void)
{
    if (!s_main) return;
    char t[16];
    int64_t us = total_us();
    fmt(t, sizeof(t), us, false);
    set_static(s_main, s_main_buf, sizeof(s_main_buf), t);
    snprintf(t, sizeof(t), ".%d", (int)(us / 100000 % 10));
    set_static(s_tenth, s_tenth_buf, sizeof(s_tenth_buf), t);
    if (s_shown_laps != s_lap_n) fill_laps();
}

static void timer_cb(lv_timer_t *t) { (void)t; update(); }

static void right_cb(lv_event_t *e)
{
    (void)e;
    int64_t now = esp_timer_get_time();
    if (s_running) {                     /* STOP */
        s_acc_us += now - s_start_us;
        s_running = false;
    } else {                             /* START / GO (continue) */
        s_start_us = now;
        s_running = true;
    }
    sound_play(SOUND_TICK);
    style_buttons();
    update();
}

static void left_cb(lv_event_t *e)
{
    (void)e;
    int64_t tot = total_us();
    if (s_running) {                     /* LAP */
        if (s_lap_n == MAX_LAPS) {       /* keep the latest */
            memmove(s_laps, s_laps + 1, sizeof(s_laps[0]) * (MAX_LAPS - 1));
            s_lap_n--;
        }
        s_laps[s_lap_n++] = tot - s_lap_from_us;
        s_lap_from_us = tot;
    } else if (tot) {                    /* RESET */
        s_acc_us = 0;
        s_lap_from_us = 0;
        s_lap_n = 0;
    }
    style_buttons();
    update();
}

static lv_obj_t *button(lv_obj_t *p, int x, lv_event_cb_t cb, lv_obj_t **lbl)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 160, 64);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0A1226), 0);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -34);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    *lbl = label(b, &lv_font_montserrat_24, COL_DARK);
    lv_obj_center(*lbl);
    return b;
}

void stopwatch_build(lv_obj_t *p)
{
    /* big time: MM:SS (or H:MM:SS) + tenths */
    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 72);
    LV_FONT_DECLARE(font_time_86);
    s_main = label(row, &font_time_86, COL_WHITE);
    s_main_buf[0] = 0;
    lv_label_set_text_static(s_main, s_main_buf);
    s_tenth = label(row, &lv_font_montserrat_48, COL_BLUE);
    s_tenth_buf[0] = 0;
    lv_label_set_text_static(s_tenth, s_tenth_buf);
    lv_obj_set_style_pad_bottom(s_tenth, 14, 0);

    /* laps */
    s_list = lv_obj_create(p);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 300, 186);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 186);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    /* buttons */
    s_left_btn = button(p, -86, left_cb, &s_left_lbl);
    lv_obj_set_style_border_width(s_left_btn, 2, 0);
    lv_obj_set_style_border_color(s_left_btn, lv_color_hex(COL_TRACK), 0);
    s_right_btn = button(p, 86, right_cb, &s_right_lbl);

    s_shown_laps = -1;
    style_buttons();
    update();
    s_timer = lv_timer_create(timer_cb, 100, NULL);   /* tenths, only while open */
}

void stopwatch_close(void)
{
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    s_main = s_tenth = s_left_btn = s_left_lbl = s_right_btn = s_right_lbl = s_list = NULL;
}
