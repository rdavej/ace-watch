/*
 * Wi-Fi picker on the watch (pull-down panel -> Wi-Fi):
 *   list of nearby networks (strongest first) -> tap one -> type the password -> Join.
 * Saved networks join straight away. The phone setup page is still there for the
 * DevX watch key ("Setup page" at the bottom).
 * Built when opened, deleted when closed (memory only while it's showing).
 */
#include "wifi_ui.h"

#include <stdio.h>
#include <string.h>
#include "net.h"

#define COL_BG      0x000000
#define COL_BLUE    0x3FB8FF
#define COL_YELLOW  0xFFD23F
#define COL_GREY    0x8C9AB8
#define COL_TRACK   0x13203D
#define COL_WHITE   0xE9EEF8
#define COL_DARK    0x00122B
#define COL_CARD    0x0A1226
#define COL_ON      0x13305A
#define COL_RED     0xFF5A5A

static lv_obj_t *s_root, *s_listv, *s_list, *s_status;
static lv_obj_t *s_pwv, *s_ta, *s_pw_msg, *s_join_btn;
static scan_ap_t s_rows[SCAN_AP_MAX];
static int s_rows_n;
static int s_scan_seen = -1, s_join_seen = -1;
static bool s_waiting_join;
static uint32_t s_close_at;
static char s_sel[33];
static char s_status_buf[64];

static lv_obj_t *label(lv_obj_t *p, const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}

static lv_obj_t *pill(lv_obj_t *p, const char *txt, bool primary, lv_event_cb_t cb, int w)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, 46);
    lv_obj_set_style_radius(b, 23, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? COL_BLUE : COL_TRACK), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? 0x7FD0FF : 0x22345A), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = label(b, &lv_font_montserrat_16, primary ? COL_DARK : COL_WHITE);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    return b;
}

static void set_status(const char *txt, uint32_t color)
{
    if (!s_status) return;
    strlcpy(s_status_buf, txt, sizeof(s_status_buf));
    lv_label_set_text_static(s_status, s_status_buf);
    lv_obj_set_style_text_color(s_status, lv_color_hex(color), 0);
}

/* ---------- password view ---------- */

static void show_list(void)
{
    if (s_pwv) { lv_obj_delete(s_pwv); s_pwv = NULL; s_ta = NULL; s_pw_msg = NULL; s_join_btn = NULL; }
    lv_obj_remove_flag(s_listv, LV_OBJ_FLAG_HIDDEN);
}

static void do_join(void)
{
    if (!s_ta) return;
    const char *pw = lv_textarea_get_text(s_ta);
    if (strlen(pw) < 8) {
        lv_label_set_text(s_pw_msg, "Wi-Fi passwords are 8+ characters");
        lv_obj_set_style_text_color(s_pw_msg, lv_color_hex(COL_YELLOW), 0);
        return;
    }
    net_wifi_join(s_sel, pw, false);
    s_waiting_join = true;
    lv_label_set_text(s_pw_msg, "Joining...");
    lv_obj_set_style_text_color(s_pw_msg, lv_color_hex(COL_GREY), 0);
}

static void join_cb(lv_event_t *e) { (void)e; do_join(); }
static void cancel_cb(lv_event_t *e) { (void)e; show_list(); }

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) do_join();
    else if (c == LV_EVENT_CANCEL) show_list();
}

static void eye_cb(lv_event_t *e)
{
    lv_obj_t *l = lv_obj_get_child(lv_event_get_target(e), 0);
    bool hidden = lv_textarea_get_password_mode(s_ta);
    lv_textarea_set_password_mode(s_ta, !hidden);
    lv_label_set_text(l, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void show_password(const char *ssid)
{
    strlcpy(s_sel, ssid, sizeof(s_sel));
    lv_obj_add_flag(s_listv, LV_OBJ_FLAG_HIDDEN);
    s_pwv = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_pwv);
    lv_obj_set_size(s_pwv, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_pwv, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = label(s_pwv, &lv_font_montserrat_14, COL_GREY);
    lv_obj_set_style_text_letter_space(t, 2, 0);
    lv_label_set_text(t, "PASSWORD FOR");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 22);
    lv_obj_t *n = label(s_pwv, &lv_font_montserrat_20, COL_BLUE);
    lv_obj_set_width(n, 330);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(n, ssid);
    lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 42);

    s_ta = lv_textarea_create(s_pwv);
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_password_mode(s_ta, true);
    lv_textarea_set_max_length(s_ta, 63);
    lv_textarea_set_placeholder_text(s_ta, "password");
    lv_obj_set_size(s_ta, 280, 48);
    lv_obj_align(s_ta, LV_ALIGN_TOP_MID, -26, 74);
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(s_ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_ta, lv_color_hex(COL_BLUE), 0);
    lv_obj_set_style_border_width(s_ta, 2, 0);
    lv_obj_set_style_radius(s_ta, 16, 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(COL_WHITE), 0);
    lv_obj_set_style_text_font(s_ta, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_ta, lv_color_hex(0x5E6C8A), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(s_ta, lv_color_hex(COL_BLUE), LV_PART_CURSOR);

    lv_obj_t *eye = lv_obj_create(s_pwv);
    lv_obj_remove_style_all(eye);
    lv_obj_set_size(eye, 48, 48);
    lv_obj_set_style_radius(eye, 16, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, 0);
    lv_obj_align(eye, LV_ALIGN_TOP_MID, 142, 74);
    lv_obj_add_event_cb(eye, eye_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *el = label(eye, &lv_font_montserrat_20, COL_GREY);
    lv_label_set_text(el, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(el);

    s_pw_msg = label(s_pwv, &lv_font_montserrat_14, COL_GREY);
    lv_label_set_text(s_pw_msg, "");
    lv_obj_align(s_pw_msg, LV_ALIGN_TOP_MID, 0, 130);

    lv_obj_t *kb = lv_keyboard_create(s_pwv);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_obj_set_size(kb, 384, 228);
    lv_obj_align(kb, LV_ALIGN_TOP_MID, 0, 152);
    lv_obj_set_style_bg_opa(kb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_obj_set_style_pad_all(kb, 0, 0);
    lv_obj_set_style_pad_gap(kb, 5, 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_TRACK), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(0x2A3B5E), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(kb, lv_color_hex(COL_CARD), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(kb, lv_color_hex(COL_WHITE), LV_PART_ITEMS);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, 10, LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_t *row = lv_obj_create(s_pwv);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -40);
    pill(row, "Cancel", false, cancel_cb, 130);
    s_join_btn = pill(row, "Join", true, join_cb, 150);
}

/* ---------- list view ---------- */

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_rows_n || s_waiting_join) return;
    scan_ap_t *a = &s_rows[i];
    char t[64];
    if (a->current) { snprintf(t, sizeof(t), "Already on %s", a->ssid); set_status(t, COL_BLUE); return; }
    if (a->saved || !a->lock) {
        strlcpy(s_sel, a->ssid, sizeof(s_sel));
        net_wifi_join(a->ssid, "", a->saved);
        s_waiting_join = true;
        snprintf(t, sizeof(t), "Joining %s...", a->ssid);
        set_status(t, COL_GREY);
        return;
    }
    show_password(a->ssid);
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    if (!net_lock(50)) return;
    s_rows_n = g_scan_ap_n;
    memcpy(s_rows, g_scan_ap, sizeof(scan_ap_t) * s_rows_n);
    net_unlock();
    for (int i = 0; i < s_rows_n; i++) {
        scan_ap_t *a = &s_rows[i];
        lv_obj_t *r = lv_obj_create(s_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, LV_PCT(100), 56);
        lv_obj_set_style_radius(r, 20, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(a->current ? COL_ON : COL_CARD), 0);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x16223F), LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
        lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *ic = label(r, &lv_font_montserrat_20, a->rssi > -70 ? COL_BLUE : COL_GREY);
        lv_label_set_text(ic, LV_SYMBOL_WIFI);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *nm = label(r, &lv_font_montserrat_16, COL_WHITE);
        lv_obj_set_width(nm, 190);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, a->ssid);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 34, 0);
        lv_obj_t *tag = label(r, &lv_font_montserrat_14, a->current ? COL_YELLOW : COL_GREY);
        lv_label_set_text(tag, a->current ? LV_SYMBOL_OK " On" : a->saved ? "Saved" : a->lock ? "" : "Open");
        lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (!s_rows_n) set_status("No networks found - tap Scan", COL_GREY);
    else set_status("Tap a network (2.4 GHz only)", COL_GREY);
}

static void scan_cb(lv_event_t *e) { (void)e; if (!s_waiting_join) { net_wifi_scan(); set_status("Looking for networks...", COL_GREY); } }
static void close_cb(lv_event_t *e) { (void)e; wifi_ui_close(); }
static void setup_cb(lv_event_t *e) { (void)e; wifi_ui_close(); net_request_setup(); }

void wifi_ui_open(void)
{
    if (s_root) return;
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE);

    s_listv = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_listv);
    lv_obj_set_size(s_listv, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_listv, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *h = label(s_listv, &lv_font_montserrat_16, COL_BLUE);
    lv_obj_set_style_text_letter_space(h, 3, 0);
    lv_label_set_text(h, "WI-FI");
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 24);
    s_status = label(s_listv, &lv_font_montserrat_14, COL_GREY);
    s_status_buf[0] = 0;
    lv_label_set_text_static(s_status, s_status_buf);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 50);

    s_list = lv_obj_create(s_listv);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 350, 290);
    lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *row = lv_obj_create(s_listv);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -58);
    pill(row, LV_SYMBOL_REFRESH " Scan", false, scan_cb, 130);
    pill(row, "Close", true, close_cb, 130);

    lv_obj_t *su = lv_obj_create(s_listv);
    lv_obj_remove_style_all(su);
    lv_obj_set_size(su, 220, 40);
    lv_obj_align(su, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_event_cb(su, setup_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = label(su, &lv_font_montserrat_14, 0x5E6C8A);
    lv_label_set_text(sl, "Setup page (DevX key)");
    lv_obj_center(sl);

    s_rows_n = 0;
    s_waiting_join = false;
    s_close_at = 0;
    s_scan_seen = s_join_seen = -1;
    set_status("Looking for networks...", COL_GREY);
    net_wifi_scan();
}

void wifi_ui_close(void)
{
    if (!s_root) return;
    lv_obj_delete_async(s_root);
    s_root = s_listv = s_list = s_status = NULL;
    s_pwv = s_ta = s_pw_msg = s_join_btn = NULL;
}

bool wifi_ui_is_open(void) { return s_root != NULL; }

bool wifi_ui_busy(void) { return s_root && (g_scan_state == REQ_BUSY || g_join_state == REQ_BUSY); }

void wifi_ui_tick(void)
{
    if (!s_root) return;
    if (s_close_at && (int32_t)(lv_tick_get() - s_close_at) >= 0) { wifi_ui_close(); return; }
    int ss = g_scan_state;
    if (ss != s_scan_seen) {
        s_scan_seen = ss;
        if (ss == REQ_OK && !s_pwv) fill_list();
        else if (ss == REQ_ERR) set_status("Wi-Fi scan failed - tap Scan", COL_YELLOW);
    }
    int js = g_join_state;
    if (s_waiting_join && js != s_join_seen) {
        s_join_seen = js;
        char t[64];
        if (js == REQ_OK) {
            s_waiting_join = false;
            show_list();
            snprintf(t, sizeof(t), LV_SYMBOL_OK " Connected to %s", s_sel);
            set_status(t, COL_YELLOW);
            s_close_at = lv_tick_get() + 1800;     /* then back to the panel's face */
        } else if (js == REQ_ERR) {
            s_waiting_join = false;
            if (s_pwv) {
                lv_label_set_text(s_pw_msg, g_join_msg[0] ? g_join_msg : "Couldn't connect");
                lv_obj_set_style_text_color(s_pw_msg, lv_color_hex(COL_RED), 0);
            } else {
                snprintf(t, sizeof(t), "%s: %s", s_sel, g_join_msg[0] ? g_join_msg : "couldn't connect");
                set_status(t, COL_RED);
            }
        }
    }
}
