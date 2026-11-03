/*
 * DevX alerts, swiped up from the bottom of the watch face (swipe down or tap the
 * bar at the top to close). Cards with the Ace icon, newest first. "Clear all"
 * hides everything up to now (remembered; new alerts still come in).
 * Built when opened, deleted when closed.
 */
#include "alerts_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "nvs.h"
#include "net.h"

#define COL_BG      0x000000
#define COL_BLUE    0x3FB8FF
#define COL_YELLOW  0xFFD23F
#define COL_GREY    0x8C9AB8
#define COL_TRACK   0x13203D
#define COL_WHITE   0xE9EEF8
#define COL_CARD    0x0A1226
#define COL_BODY    0xB7C2D9

LV_IMAGE_DECLARE(img_ace_small);

static lv_obj_t *s_root, *s_list, *s_new_lbl, *s_status, *s_clear;
static uint32_t s_rev = 0xFFFFFFFF;
static int s_state = -1;
static int64_t s_cleared_at = -1;     /* alerts up to this time (server ms) were cleared */

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}

static void cleared_load(void)
{
    if (s_cleared_at >= 0) return;
    s_cleared_at = 0;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i64(h, "clr", &s_cleared_at);
        nvs_close(h);
    }
}

static void when(char *out, size_t n, int64_t at_ms)
{
    time_t now = time(NULL), t = (time_t)(at_ms / 1000);
    if (now < 1700000000 || at_ms <= 0) { out[0] = 0; return; }
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) {
        int h = a.tm_hour % 12;
        snprintf(out, n, "%d:%02d %s", h ? h : 12, a.tm_min, a.tm_hour < 12 ? "am" : "pm");
    } else if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1) {
        snprintf(out, n, "Yest.");
    } else {
        static const char *mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        snprintf(out, n, "%d %s", a.tm_mday, mo[a.tm_mon]);
    }
}

static void fill(void)
{
    lv_obj_clean(s_list);
    if (!net_lock(20)) return;
    int shown = 0;
    for (int i = 0; i < g_feed_n; i++) {
        const feed_t *f = &g_feed[i];
        if (f->at <= s_cleared_at) continue;
        shown++;
        lv_obj_t *card = lv_obj_create(s_list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_hex(COL_CARD), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x1E3A66), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 22, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_style_pad_column(card, 10, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *ic = lv_image_create(card);
        lv_image_set_src(ic, &img_ace_small);
        lv_image_set_scale(ic, 192);             /* 48 px -> ~36 px */
        lv_obj_set_size(ic, 36, 40);
        lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_CENTER);

        lv_obj_t *col = lv_obj_create(card);
        lv_obj_remove_style_all(col);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_height(col, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(col, 3, 0);
        lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);

        lv_obj_t *top = lv_obj_create(col);
        lv_obj_remove_style_all(top);
        lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(top, LV_OBJ_FLAG_GESTURE_BUBBLE);
        char w[16];
        when(w, sizeof(w), f->at);
        lv_obj_t *tm = label(top, &lv_font_montserrat_14, COL_GREY);
        lv_label_set_text(tm, w);
        lv_obj_align(tm, LV_ALIGN_TOP_RIGHT, 0, 1);
        lv_obj_t *tt = label(top, &lv_font_montserrat_16, COL_WHITE);
        lv_obj_set_width(tt, 196);
        lv_label_set_long_mode(tt, LV_LABEL_LONG_WRAP);
        lv_label_set_text(tt, f->title);

        lv_obj_t *bd = label(col, &lv_font_montserrat_14, COL_BODY);
        lv_obj_set_width(bd, LV_PCT(100));
        lv_label_set_long_mode(bd, LV_LABEL_LONG_WRAP);
        lv_label_set_text(bd, f->body);
    }
    s_rev = g_feed_rev;
    net_unlock();
    if (!shown) {
        lv_obj_t *e = label(s_list, &lv_font_montserrat_16, COL_GREY);
        lv_obj_set_width(e, LV_PCT(100));
        lv_obj_set_style_text_align(e, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(e, 60, 0);
        lv_label_set_text(e, "All caught up");
        lv_obj_add_flag(s_clear, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_clear, LV_OBJ_FLAG_HIDDEN);
    }
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in && lv_indev_get_gesture_dir(in) == LV_DIR_BOTTOM) {
        lv_indev_wait_release(in);
        alerts_ui_close();
    }
}

static void close_cb(lv_event_t *e) { (void)e; alerts_ui_close(); }

static void clear_cb(lv_event_t *e)
{
    (void)e;
    int64_t newest = s_cleared_at;
    if (net_lock(20)) {
        for (int i = 0; i < g_feed_n; i++) if (g_feed[i].at > newest) newest = g_feed[i].at;
        net_unlock();
    }
    s_cleared_at = newest;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "clr", s_cleared_at);
        nvs_commit(h);
        nvs_close(h);
    }
    lv_label_set_text(s_new_lbl, "");
    fill();
}

void alerts_ui_open(int unread)
{
    if (s_root) return;
    cleared_load();
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_root, gesture_cb, LV_EVENT_GESTURE, NULL);

    /* handle at the top: tap (or swipe down) to close */
    lv_obj_t *hz = lv_obj_create(s_root);
    lv_obj_remove_style_all(hz);
    lv_obj_set_size(hz, 160, 34);
    lv_obj_align(hz, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_add_flag(hz, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(hz, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bar = lv_obj_create(hz);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 44, 4);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x2A3B5E), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *t = label(s_root, &lv_font_montserrat_16, COL_BLUE);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_label_set_text(t, "ALERTS");
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 44, 40);
    s_new_lbl = label(s_root, &lv_font_montserrat_14, COL_YELLOW);
    if (unread > 0) lv_label_set_text_fmt(s_new_lbl, "%d NEW", unread);
    else lv_label_set_text(s_new_lbl, "");
    lv_obj_align(s_new_lbl, LV_ALIGN_TOP_RIGHT, -44, 42);
    s_status = label(s_root, &lv_font_montserrat_14, COL_GREY);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 64);

    s_list = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 366, 318);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_clear = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_clear);
    lv_obj_set_size(s_clear, 150, 44);
    lv_obj_set_style_radius(s_clear, 22, 0);
    lv_obj_set_style_bg_color(s_clear, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_color(s_clear, lv_color_hex(0x22345A), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_clear, LV_OPA_COVER, 0);
    lv_obj_align(s_clear, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(s_clear, clear_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cl = label(s_clear, &lv_font_montserrat_16, COL_WHITE);
    lv_label_set_text(cl, "Clear all");
    lv_obj_center(cl);

    s_state = -1;
    fill();
    if (net_has_key()) net_feed_refresh();
    else lv_label_set_text(s_status, "Add your DevX watch key first");
    g_feed_new = 0;
}

void alerts_ui_close(void)
{
    if (!s_root) return;
    lv_obj_delete_async(s_root);
    s_root = s_list = s_new_lbl = s_status = s_clear = NULL;
}

bool alerts_ui_is_open(void) { return s_root != NULL; }

void alerts_ui_tick(void)
{
    if (!s_root) return;
    if (s_rev != g_feed_rev) fill();
    if (s_state != (int)g_feed_state && net_has_key()) {
        s_state = g_feed_state;
        lv_label_set_text(s_status, s_state == REQ_BUSY ? LV_SYMBOL_WIFI " Checking..." :
                                    s_state == REQ_ERR ? "Couldn't reach DevX - showing saved" : "");
    }
    g_feed_new = 0;          /* looking at them: no pop-up on top */
}
