/*
 * Workout screen
 *   - Rest ring: counts down the rest between sets
 *   - Presets: 60 s / 90 s / 2 min (tap to pick; restarts a running rest)
 *   - SET DONE: adds a set and starts the rest
 *   - Hold SET DONE 3 s: end the workout (resets sets + clock)
 *   - Workout clock starts with the first set
 */
#include "workout.h"

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

#define GO_SHOW_MS  10000   /* "GO!" stays yellow this long */

static const int s_presets[3] = {60, 90, 120};
static const char *s_preset_txt[3] = {"60s", "90s", "2m"};

static lv_obj_t *s_panel;
static lv_obj_t *s_elapsed, *s_sets, *s_arc, *s_count, *s_caption;
static lv_obj_t *s_pbtn[3], *s_plbl[3];
static lv_obj_t *s_done_btn;

static char s_elapsed_buf[12], s_sets_buf[16], s_count_buf[8], s_caption_buf[16];

static int s_preset = 1;            /* 90 s */
static int s_set_count = 0;
static int64_t s_start_us = 0;      /* workout start, 0 = not started */
static int64_t s_rest_end_us = 0;   /* 0 = not resting */
static int s_rest_total = 90;
static int64_t s_go_until_us = 0;   /* showing "GO!" */
static bool s_ended_by_hold = false;
static int s_last_tick = -1;      /* last countdown second that beeped */

static int64_t now_us(void) { return esp_timer_get_time(); }

static void set_static(lv_obj_t *l, char *buf, size_t n, const char *txt)
{
    if (lv_label_get_text(l) == buf && strcmp(buf, txt) == 0) return;
    strlcpy(buf, txt, n);
    lv_label_set_text_static(l, buf);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static void ring_color(uint32_t c)
{
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(c), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_count, lv_color_hex(c == COL_BLUE ? COL_WHITE : c), 0);
    lv_obj_set_style_text_color(s_caption, lv_color_hex(c == COL_BLUE ? COL_GREY : c), 0);
}

static void style_presets(void)
{
    for (int i = 0; i < 3; i++) {
        bool on = i == s_preset;
        lv_obj_set_style_bg_color(s_pbtn[i], lv_color_hex(on ? COL_BLUE : COL_BG), 0);
        lv_obj_set_style_border_color(s_pbtn[i], lv_color_hex(on ? COL_BLUE : COL_TRACK), 0);
        lv_obj_set_style_text_color(s_plbl[i], lv_color_hex(on ? COL_DARK : COL_GREY), 0);
    }
}

static void show_idle(void)
{
    char t[8];
    int s = s_presets[s_preset];
    snprintf(t, sizeof(t), "%d:%02d", s / 60, s % 60);
    set_static(s_count, s_count_buf, sizeof(s_count_buf), t);
    set_static(s_caption, s_caption_buf, sizeof(s_caption_buf), "TAP TO REST");
    lv_arc_set_value(s_arc, 1000);
    ring_color(COL_BLUE);
}

static void start_rest(void)
{
    s_rest_total = s_presets[s_preset];
    s_rest_end_us = now_us() + (int64_t)s_rest_total * 1000000;
    s_go_until_us = 0;
    s_last_tick = -1;
    if (!s_start_us) s_start_us = now_us();
    set_static(s_caption, s_caption_buf, sizeof(s_caption_buf), "REST");
    ring_color(COL_BLUE);
    workout_tick();
}

static void preset_cb(lv_event_t *e)
{
    s_preset = (int)(intptr_t)lv_event_get_user_data(e);
    style_presets();
    if (s_rest_end_us) start_rest();   /* resting: restart with the new time */
    else show_idle();
}

static void ring_cb(lv_event_t *e)
{
    (void)e;
    start_rest();                      /* tap the ring: start / restart rest */
}

static void done_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        /* hold 3 s: end workout */
        s_set_count = 0;
        s_start_us = 0;
        s_rest_end_us = 0;
        s_go_until_us = 0;
        s_ended_by_hold = true;
        set_static(s_sets, s_sets_buf, sizeof(s_sets_buf), "0 SETS");
        set_static(s_elapsed, s_elapsed_buf, sizeof(s_elapsed_buf), "0:00");
        show_idle();
        return;
    }
    if (code == LV_EVENT_CLICKED) {
        if (s_ended_by_hold) { s_ended_by_hold = false; return; } /* release after the hold */
        s_set_count++;
        char t[16];
        snprintf(t, sizeof(t), "%d SET%s", s_set_count, s_set_count == 1 ? "" : "S");
        set_static(s_sets, s_sets_buf, sizeof(s_sets_buf), t);
        start_rest();
    }
}

bool workout_tick(void)
{
    bool finished = false;
    int64_t now = now_us();

    if (s_start_us) {
        int sec = (int)((now - s_start_us) / 1000000);
        char t[12];
        if (sec >= 3600) snprintf(t, sizeof(t), "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
        else snprintf(t, sizeof(t), "%d:%02d", sec / 60, sec % 60);
        set_static(s_elapsed, s_elapsed_buf, sizeof(s_elapsed_buf), t);
    }

    if (s_rest_end_us) {
        int64_t left_us = s_rest_end_us - now;
        if (left_us <= 0) {
            s_rest_end_us = 0;
            s_go_until_us = now + GO_SHOW_MS * 1000LL;
            set_static(s_count, s_count_buf, sizeof(s_count_buf), "0:00");
            set_static(s_caption, s_caption_buf, sizeof(s_caption_buf), "GO!");
            lv_arc_set_value(s_arc, 0);
            ring_color(COL_YELLOW);
            sound_play(SOUND_GO);
            finished = true;
        } else {
            int left = (int)((left_us + 999999) / 1000000);   /* round up */
            if (left <= 3 && left != s_last_tick) {            /* tick at 3, 2, 1 */
                s_last_tick = left;
                sound_play(SOUND_TICK);
            }
            char t[8];
            snprintf(t, sizeof(t), "%d:%02d", left / 60, left % 60);
            set_static(s_count, s_count_buf, sizeof(s_count_buf), t);
            lv_arc_set_value(s_arc, (int32_t)(left_us / 1000 / s_rest_total));  /* 0..1000 */
        }
    } else if (s_go_until_us && now > s_go_until_us) {
        s_go_until_us = 0;
        show_idle();
    }
    return finished;
}

void workout_show(bool show)
{
    if (show) lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

bool workout_visible(void)
{
    return !lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void workout_build(lv_obj_t *scr)
{
    s_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* Header */
    lv_obj_t *h = label(s_panel, &lv_font_montserrat_20, COL_BLUE);
    lv_label_set_text(h, "WORKOUT");
    lv_obj_set_style_text_letter_space(h, 4, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 36);

    /* Clock + sets */
    s_elapsed = label(s_panel, &lv_font_montserrat_24, COL_WHITE);
    set_static(s_elapsed, s_elapsed_buf, sizeof(s_elapsed_buf), "0:00");
    lv_obj_align(s_elapsed, LV_ALIGN_TOP_MID, -70, 70);

    s_sets = label(s_panel, &lv_font_montserrat_24, COL_YELLOW);
    set_static(s_sets, s_sets_buf, sizeof(s_sets_buf), "0 SETS");
    lv_obj_align(s_sets, LV_ALIGN_TOP_MID, 70, 70);

    /* Rest ring */
    s_arc = lv_arc_create(s_panel);
    lv_obj_set_size(s_arc, 220, 220);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_arc_set_value(s_arc, 1000);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 14, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_arc, 0, 0);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_arc, LV_ALIGN_CENTER, 0, -10);
    /* not draggable like a slider: the circle below takes the taps */
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    /* A plain clickable circle on top of the ring takes the taps */
    lv_obj_t *hit = lv_obj_create(s_panel);
    lv_obj_remove_style_all(hit);
    lv_obj_set_size(hit, 220, 220);
    lv_obj_set_style_radius(hit, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(hit, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_event_cb(hit, ring_cb, LV_EVENT_CLICKED, NULL);

    s_count = label(hit, &lv_font_montserrat_48, COL_WHITE);
    lv_obj_align(s_count, LV_ALIGN_CENTER, 0, -8);
    s_caption = label(hit, &lv_font_montserrat_16, COL_GREY);
    lv_obj_set_style_text_letter_space(s_caption, 2, 0);
    lv_obj_align(s_caption, LV_ALIGN_CENTER, 0, 36);

    /* Presets */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_obj_create(s_panel);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 92, 44);
        lv_obj_set_style_radius(b, 22, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_align(b, LV_ALIGN_CENTER, (i - 1) * 104, 136);
        lv_obj_add_event_cb(b, preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *l = label(b, &lv_font_montserrat_20, COL_GREY);
        lv_label_set_text_static(l, s_preset_txt[i]);
        lv_obj_center(l);
        s_pbtn[i] = b;
        s_plbl[i] = l;
    }

    /* SET DONE */
    s_done_btn = lv_obj_create(s_panel);
    lv_obj_remove_style_all(s_done_btn);
    lv_obj_set_size(s_done_btn, 280, 62);
    lv_obj_set_style_radius(s_done_btn, 31, 0);
    lv_obj_set_style_bg_color(s_done_btn, lv_color_hex(COL_YELLOW), 0);
    lv_obj_set_style_bg_opa(s_done_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_done_btn, lv_color_hex(0xC9A42F), LV_STATE_PRESSED);
    lv_obj_align(s_done_btn, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_add_event_cb(s_done_btn, done_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_done_btn, done_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *dl = label(s_done_btn, &lv_font_montserrat_24, COL_DARK);
    lv_label_set_text(dl, LV_SYMBOL_PLUS "  SET DONE");
    lv_obj_center(dl);

    style_presets();
    show_idle();
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}
