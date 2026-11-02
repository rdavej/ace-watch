/*
 * Extra screens, built when you swipe to them and deleted when you leave
 * (so they only use memory while they're showing):
 *   Steps     last 7 days as bars
 *   Alerts    DevX notifications (copy of the Director's pushes)
 *   Switches  Smart Life switches + gym gate, via the DevX server
 *   Settings  brightness, sound, screen-off time
 */
#include "pages.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp/esp-bsp.h"
#include "esp_app_desc.h"
#include "motion.h"
#include "net.h"
#include "settings.h"
#include "sound.h"
#include "stopwatch.h"

#define COL_BG      0x000000
#define COL_BLUE    0x3FB8FF
#define COL_YELLOW  0xFFD23F
#define COL_GREY    0x8C9AB8
#define COL_TRACK   0x13203D
#define COL_WHITE   0xE9EEF8
#define COL_DARK    0x00122B
#define COL_RED     0xFF5A5A

static int s_kind = -1;
static lv_obj_t *s_root;
static lv_obj_t *s_status;     /* status line at the bottom (alerts, switches) */
static lv_obj_t *s_list;       /* list container (alerts, switches) */
static uint32_t s_rev;
static int s_state = -1;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *panel(lv_obj_t *scr, const char *title)
{
    lv_obj_t *p = lv_obj_create(scr);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *h = label(p, &lv_font_montserrat_20, COL_BLUE);
    lv_label_set_text(h, title);
    lv_obj_set_style_text_letter_space(h, 4, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 36);
    return p;
}

static lv_obj_t *list_box(lv_obj_t *p, int y, int h)
{
    lv_obj_t *l = lv_obj_create(p);
    lv_obj_remove_style_all(l);
    lv_obj_set_size(l, 360, h);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(l, 10, 0);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    return l;
}

static lv_obj_t *status_line(lv_obj_t *p)
{
    lv_obj_t *s = label(p, &lv_font_montserrat_16, COL_GREY);
    lv_label_set_text(s, "");
    lv_obj_align(s, LV_ALIGN_BOTTOM_MID, 0, -40);
    return s;
}

static void no_key_note(lv_obj_t *p)
{
    lv_obj_t *n = label(p, &lv_font_montserrat_20, COL_GREY);
    lv_obj_set_width(n, 330);
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(n, "Add your DevX watch key first:\n\nhold the watch face 3 s,\nthen open the setup page\non your phone.");
    lv_obj_center(n);
}

/* ======================= Steps ======================= */

static int weekday_of(int32_t code)   /* code = yy*1000 + day of year (0-based) */
{
    struct tm t = {0};
    t.tm_year = (int)(code / 1000) + 100;
    t.tm_mon = 0;
    t.tm_mday = (int)(code % 1000) + 1;
    t.tm_hour = 12;
    mktime(&t);
    return t.tm_wday;
}

static void build_steps(lv_obj_t *p)
{
    static const char *WD = "SMTWTFS";
    uint32_t val[7];
    int32_t day[7];
    /* oldest on the left, today on the right */
    val[6] = g_steps_today;
    time_t now = time(NULL);
    struct tm tn;
    localtime_r(&now, &tn);
    day[6] = (tn.tm_year % 100) * 1000 + tn.tm_yday;
    for (int i = 0; i < 6; i++) {
        val[5 - i] = g_step_history[i].steps;
        day[5 - i] = g_step_history[i].day;
    }

    char t[24];
    lv_obj_t *big = label(p, &lv_font_montserrat_48, COL_YELLOW);
    snprintf(t, sizeof(t), "%lu", (unsigned long)g_steps_today);
    lv_label_set_text(big, t);
    lv_obj_align(big, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *sub = label(p, &lv_font_montserrat_16, COL_GREY);
    uint32_t sum = 0;
    int days = 0;
    for (int i = 0; i < 6; i++) if (day[i]) { sum += val[i]; days++; }
    if (days) snprintf(t, sizeof(t), "today  -  avg %lu", (unsigned long)(sum / days));
    else snprintf(t, sizeof(t), "today");
    lv_label_set_text(sub, t);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 124);

    uint32_t top = STEP_GOAL;
    for (int i = 0; i < 7; i++) if (val[i] > top) top = val[i];

    const int H = 190, BASE_Y = 380, W = 30, GAP = 18;
    int x0 = (410 - (7 * W + 6 * GAP)) / 2;

    /* goal line */
    lv_obj_t *gl = lv_obj_create(p);
    lv_obj_remove_style_all(gl);
    lv_obj_set_size(gl, 7 * W + 6 * GAP + 16, 2);
    lv_obj_set_style_bg_color(gl, lv_color_hex(COL_TRACK), 0);
    lv_obj_set_style_bg_opa(gl, LV_OPA_COVER, 0);
    lv_obj_set_pos(gl, x0 - 8, BASE_Y - (int)((uint64_t)STEP_GOAL * H / top));

    for (int i = 0; i < 7; i++) {
        int h = (int)((uint64_t)val[i] * H / top);
        if (h < 6) h = 6;
        int x = x0 + i * (W + GAP);
        lv_obj_t *b = lv_obj_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, W, h);
        lv_obj_set_style_radius(b, 8, 0);
        uint32_t c = i == 6 ? COL_YELLOW : val[i] >= STEP_GOAL ? COL_BLUE : 0x2E6FA0;
        lv_obj_set_style_bg_color(b, lv_color_hex(day[i] || i == 6 ? c : COL_TRACK), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_pos(b, x, BASE_Y - h);

        lv_obj_t *d = label(p, &lv_font_montserrat_16, i == 6 ? COL_YELLOW : COL_GREY);
        char w[2] = { day[i] ? WD[weekday_of(day[i])] : '-', 0 };
        lv_label_set_text(d, w);
        lv_obj_set_pos(d, x + W / 2 - 6, BASE_Y + 8);

        if (day[i] || i == 6) {
            lv_obj_t *v = label(p, &lv_font_montserrat_16, COL_GREY);
            if (val[i] >= 1000) snprintf(t, sizeof(t), "%lu.%luk", (unsigned long)(val[i] / 1000), (unsigned long)(val[i] % 1000 / 100));
            else snprintf(t, sizeof(t), "%lu", (unsigned long)val[i]);
            lv_label_set_text(v, t);
            lv_obj_set_width(v, W + GAP);
            lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(v, x - GAP / 2, BASE_Y - h - 22);
        }
    }
}

/* ======================= Alerts ======================= */

static void fill_alerts(void)
{
    lv_obj_clean(s_list);
    if (!net_lock(20)) return;
    int n = g_feed_n;
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    for (int i = 0; i < n; i++) {
        lv_obj_t *it = lv_obj_create(s_list);
        lv_obj_remove_style_all(it);
        lv_obj_set_size(it, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(it, lv_color_hex(0x0A1226), 0);
        lv_obj_set_style_bg_opa(it, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(it, 16, 0);
        lv_obj_set_style_pad_all(it, 12, 0);
        lv_obj_set_flex_flow(it, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(it, 4, 0);
        lv_obj_remove_flag(it, LV_OBJ_FLAG_CLICKABLE);

        char ago[16];
        int64_t m = (now_ms - g_feed[i].at) / 60000;
        if (now_ms < 1700000000000LL || m < 0) ago[0] = 0;
        else if (m < 1) snprintf(ago, sizeof(ago), "now");
        else if (m < 60) snprintf(ago, sizeof(ago), "%dm ago", (int)m);
        else if (m < 48 * 60) snprintf(ago, sizeof(ago), "%dh ago", (int)(m / 60));
        else snprintf(ago, sizeof(ago), "%dd ago", (int)(m / 1440));

        lv_obj_t *a = label(it, &lv_font_montserrat_16, COL_YELLOW);
        lv_label_set_text(a, ago);
        lv_obj_t *tt = label(it, &lv_font_montserrat_20, COL_WHITE);
        lv_obj_set_width(tt, LV_PCT(100));
        lv_label_set_long_mode(tt, LV_LABEL_LONG_WRAP);
        lv_label_set_text(tt, g_feed[i].title);
        if (g_feed[i].body[0]) {
            lv_obj_t *bd = label(it, &lv_font_montserrat_16, COL_GREY);
            lv_obj_set_width(bd, LV_PCT(100));
            lv_label_set_long_mode(bd, LV_LABEL_LONG_WRAP);
            lv_label_set_text(bd, g_feed[i].body);
        }
    }
    s_rev = g_feed_rev;
    net_unlock();
    if (!n) {
        lv_obj_t *e = label(s_list, &lv_font_montserrat_20, COL_GREY);
        lv_label_set_text(e, "No alerts yet");
    }
}

static void build_alerts(lv_obj_t *p)
{
    if (!net_has_key()) { no_key_note(p); return; }
    s_list = list_box(p, 76, 350);
    s_status = status_line(p);
    g_feed_new = 0;
    fill_alerts();
    net_feed_refresh();
}

static void tick_alerts(void)
{
    if (!s_list) return;
    if (s_rev != g_feed_rev) fill_alerts();
    if (s_state != g_feed_state) {
        s_state = g_feed_state;
        lv_label_set_text(s_status, s_state == REQ_BUSY ? LV_SYMBOL_WIFI " Checking..." :
                                    s_state == REQ_ERR ? "Couldn't check - try later" : "");
    }
    g_feed_new = 0;
}

/* ======================= Switches ======================= */

static lv_obj_t *s_gate_btn, *s_gate_lbl;
static uint32_t s_gate_armed_until;
static int s_gate_state = -1;

static void sw_tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    char id[24];
    bool on;
    if (!net_lock(20)) return;
    if (i >= g_sw_n) { net_unlock(); return; }
    strlcpy(id, g_sw[i].id, sizeof(id));
    on = g_sw[i].on != 1;
    net_unlock();
    lv_obj_t *pill = lv_obj_get_child(lv_event_get_target_obj(e), 1);
    if (pill) {
        lv_obj_t *pl = lv_obj_get_child(pill, 0);
        if (pl) lv_label_set_text(pl, "...");
    }
    net_switch_set(id, on);
}

static void gate_cb(lv_event_t *e)
{
    (void)e;
    uint32_t now = lv_tick_get();
    if (s_gate_armed_until && (int32_t)(s_gate_armed_until - now) > 0) {
        s_gate_armed_until = 0;
        net_gate_open();
        lv_label_set_text(s_gate_lbl, "Opening...");
    } else {
        s_gate_armed_until = now + 3000;   /* tap twice so it can't open by accident */
        lv_label_set_text(s_gate_lbl, "Tap again to open");
    }
}

static void fill_switches(void)
{
    lv_obj_clean(s_list);
    s_gate_btn = NULL;
    if (!net_lock(20)) return;
    int n = g_sw_n;
    bool gate = false;
    for (int i = 0; i < n; i++) if (g_sw[i].gate) gate = true;

    if (gate) {
        s_gate_btn = lv_obj_create(s_list);
        lv_obj_remove_style_all(s_gate_btn);
        lv_obj_set_size(s_gate_btn, LV_PCT(100), 60);
        lv_obj_set_style_radius(s_gate_btn, 30, 0);
        lv_obj_set_style_bg_color(s_gate_btn, lv_color_hex(COL_YELLOW), 0);
        lv_obj_set_style_bg_opa(s_gate_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_gate_btn, lv_color_hex(0xC9A42F), LV_STATE_PRESSED);
        lv_obj_add_event_cb(s_gate_btn, gate_cb, LV_EVENT_CLICKED, NULL);
        s_gate_lbl = label(s_gate_btn, &lv_font_montserrat_24, COL_DARK);
        lv_label_set_text(s_gate_lbl, LV_SYMBOL_HOME "  OPEN GATE");
        lv_obj_center(s_gate_lbl);
        s_gate_state = -1;
    }

    for (int i = 0; i < n; i++) {
        if (g_sw[i].gate) continue;
        lv_obj_t *row = lv_obj_create(s_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_PCT(100), 58);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0A1226), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x16223F), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_add_event_cb(row, sw_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *nm = label(row, &lv_font_montserrat_20, g_sw[i].online ? COL_WHITE : COL_GREY);
        lv_obj_set_width(nm, 230);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_label_set_text(nm, g_sw[i].name);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 16, 0);

        bool on = g_sw[i].on == 1;
        lv_obj_t *pill = lv_obj_create(row);
        lv_obj_remove_style_all(pill);
        lv_obj_set_size(pill, 76, 36);
        lv_obj_set_style_radius(pill, 18, 0);
        lv_obj_set_style_border_width(pill, 2, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(on ? COL_BLUE : COL_TRACK), 0);
        lv_obj_set_style_bg_color(pill, lv_color_hex(on ? COL_BLUE : COL_BG), 0);
        lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(pill, LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_t *pl = label(pill, &lv_font_montserrat_16, on ? COL_DARK : COL_GREY);
        lv_label_set_text(pl, !g_sw[i].online ? "offline" : g_sw[i].on < 0 ? "?" : on ? "ON" : "OFF");
        lv_obj_center(pl);
    }
    s_rev = g_sw_rev;
    net_unlock();
    if (!n) {
        lv_obj_t *e = label(s_list, &lv_font_montserrat_20, COL_GREY);
        lv_label_set_text(e, g_sw_state == REQ_ERR ? "" : "Loading...");
    }
}

static void build_switches(lv_obj_t *p)
{
    if (!net_has_key()) { no_key_note(p); return; }
    s_list = list_box(p, 76, 350);
    s_status = status_line(p);
    s_gate_armed_until = 0;
    fill_switches();
    net_switches_refresh();
}

static void tick_switches(void)
{
    if (!s_list) return;
    if (s_rev != g_sw_rev) fill_switches();
    if (s_state != g_sw_state) {
        s_state = g_sw_state;
        lv_label_set_text(s_status, s_state == REQ_BUSY ? LV_SYMBOL_WIFI " Connecting..." :
                                    s_state == REQ_ERR ? g_sw_msg : "");
        if (s_state == REQ_ERR) fill_switches();   /* undo the "..." */
    }
    if (s_gate_btn) {
        if (s_gate_state != g_gate_state) {
            s_gate_state = g_gate_state;
            if (s_gate_state == REQ_OK || s_gate_state == REQ_ERR) {
                lv_label_set_text(s_gate_lbl, g_gate_msg);
                s_gate_armed_until = lv_tick_get() + 4000;   /* show the result a few seconds */
            }
        }
        if (s_gate_armed_until && (int32_t)(lv_tick_get() - s_gate_armed_until) > 0) {
            s_gate_armed_until = 0;
            if (g_gate_state != REQ_BUSY) {
                g_gate_state = REQ_IDLE;
                s_gate_state = REQ_IDLE;
                lv_label_set_text(s_gate_lbl, LV_SYMBOL_HOME "  OPEN GATE");
            }
        }
    }
}

/* ======================= Settings ======================= */

static lv_obj_t *s_opt[3][4];
static const char *const OPT_TXT[3][4] = {
    {"Low", "Mid", "High", "Max"},
    {"Off", "Low", "High", NULL},
    {"10s", "20s", "30s", NULL},
};

static void style_opts(void)
{
    const uint8_t cur[3] = { g_set.bright, g_set.volume, g_set.timeout };
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 4; i++) {
            lv_obj_t *b = s_opt[r][i];
            if (!b) continue;
            bool on = cur[r] == i;
            lv_obj_set_style_bg_color(b, lv_color_hex(on ? COL_BLUE : COL_BG), 0);
            lv_obj_set_style_border_color(b, lv_color_hex(on ? COL_BLUE : COL_TRACK), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(on ? COL_DARK : COL_GREY), 0);
        }
    }
}

static void opt_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int r = v / 10, i = v % 10;
    if (r == 0) { g_set.bright = i; bsp_display_brightness_set(settings_brightness()); }
    if (r == 1) { g_set.volume = i; sound_play(SOUND_TICK); }
    if (r == 2) { g_set.timeout = i; }
    settings_save();
    style_opts();
}

static void build_settings(lv_obj_t *p)
{
    static const char *const ROW[3] = { LV_SYMBOL_IMAGE "  Brightness", LV_SYMBOL_VOLUME_MAX "  Sound", LV_SYMBOL_EYE_CLOSE "  Screen off after" };
    memset(s_opt, 0, sizeof(s_opt));
    for (int r = 0; r < 3; r++) {
        int y = 86 + r * 98;
        lv_obj_t *t = label(p, &lv_font_montserrat_16, COL_GREY);
        lv_label_set_text(t, ROW[r]);
        lv_obj_set_pos(t, 30, y);
        int cnt = OPT_TXT[r][3] ? 4 : 3;
        int w = cnt == 4 ? 80 : 108, gap = 8;
        int x0 = (410 - (cnt * w + (cnt - 1) * gap)) / 2;
        for (int i = 0; i < cnt; i++) {
            lv_obj_t *b = lv_obj_create(p);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, w, 46);
            lv_obj_set_style_radius(b, 23, 0);
            lv_obj_set_style_border_width(b, 2, 0);
            lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
            lv_obj_set_pos(b, x0 + i * (w + gap), y + 26);
            lv_obj_add_event_cb(b, opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(r * 10 + i));
            lv_obj_t *l = label(b, &lv_font_montserrat_20, COL_GREY);
            lv_label_set_text(l, OPT_TXT[r][i]);
            lv_obj_center(l);
            s_opt[r][i] = b;
        }
    }
    style_opts();

    char t[64];
    lv_obj_t *f = label(p, &lv_font_montserrat_16, COL_GREY);
    snprintf(t, sizeof(t), "Ace Watch v%s\nDevX key: %s", esp_app_get_description()->version,
             net_has_key() ? "set" : "not set (hold face 3 s)");
    lv_label_set_text(f, t);
    lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, -36);
}

/* ======================= Ace ======================= */

static lv_obj_t *s_ace_text, *s_ace_btn, *s_ace_lbl, *s_ace_status;
static uint32_t s_ace_rev_shown;
static int s_ace_state_shown = -1;
static char s_ace_buf[1100];

static void ace_btn_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        if (g_ace_state == ACE_THINK || g_ace_state == ACE_LISTEN || g_ace_state == ACE_CONNECT) return;
        g_ace_hold = true;
        net_ace_start();
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        g_ace_hold = false;
    }
}

LV_IMAGE_DECLARE(img_ace_big);

static void ace_shield(lv_obj_t *p, int x, int y)
{
    lv_obj_t *img = lv_image_create(p);
    lv_image_set_src(img, &img_ace_big);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(img, LV_ALIGN_TOP_MID, x, y);
}

static void build_ace(lv_obj_t *p)
{
    if (!net_has_key()) { no_key_note(p); return; }
    ace_shield(p, 0, 52);

    /* conversation: what Ace heard + his answer (scrolls) */
    lv_obj_t *box = lv_obj_create(p);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 340, 160);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_OFF);
    s_ace_text = label(box, &lv_font_montserrat_20, COL_WHITE);
    lv_obj_set_width(s_ace_text, 340);
    lv_label_set_long_mode(s_ace_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ace_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_ace_text, "Hold the button and ask me anything.\n\"Who's in the gym?\"  \"Switch off the office lights\"");

    s_ace_status = label(p, &lv_font_montserrat_16, COL_GREY);
    lv_label_set_text(s_ace_status, "");
    lv_obj_align(s_ace_status, LV_ALIGN_BOTTOM_MID, 0, -128);

    /* hold to talk */
    s_ace_btn = lv_obj_create(p);
    lv_obj_remove_style_all(s_ace_btn);
    lv_obj_set_size(s_ace_btn, 220, 72);
    lv_obj_set_style_radius(s_ace_btn, 36, 0);
    lv_obj_set_style_bg_opa(s_ace_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_ace_btn, lv_color_hex(COL_BLUE), 0);
    lv_obj_align(s_ace_btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_remove_flag(s_ace_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);   /* a wobble while holding isn't a swipe */
    lv_obj_add_event_cb(s_ace_btn, ace_btn_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ace_btn, ace_btn_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_ace_btn, ace_btn_cb, LV_EVENT_PRESS_LOST, NULL);
    s_ace_lbl = label(s_ace_btn, &lv_font_montserrat_20, COL_DARK);
    lv_label_set_text(s_ace_lbl, LV_SYMBOL_AUDIO "  HOLD TO TALK");
    lv_obj_center(s_ace_lbl);

    s_ace_state_shown = -1;
    s_ace_rev_shown = g_ace_rev;
    if (g_ace_state == ACE_DONE || g_ace_state == ACE_ERR) s_ace_rev_shown = g_ace_rev - 1;   /* show the last answer */
    net_warm_up();     /* connect now, so talking starts straight away */
}

static void tick_ace(void)
{
    if (!s_ace_btn) return;
    int st = g_ace_state;
    if (st != s_ace_state_shown) {
        s_ace_state_shown = st;
        bool busy = st == ACE_CONNECT || st == ACE_LISTEN || st == ACE_THINK;
        lv_obj_set_style_bg_color(s_ace_btn, lv_color_hex(st == ACE_LISTEN ? COL_YELLOW : busy ? COL_TRACK : COL_BLUE), 0);
        lv_obj_set_style_text_color(s_ace_lbl, lv_color_hex(busy && st != ACE_LISTEN ? COL_GREY : COL_DARK), 0);
        lv_label_set_text(s_ace_lbl, st == ACE_LISTEN ? LV_SYMBOL_AUDIO "  LISTENING" :
                                     st == ACE_CONNECT ? LV_SYMBOL_WIFI "  CONNECTING" :
                                     st == ACE_THINK ? "THINKING..." : LV_SYMBOL_AUDIO "  HOLD TO TALK");
        lv_label_set_text(s_ace_status, st == ACE_LISTEN ? "Let go when you're done" :
                                        st == ACE_CONNECT ? "Keep holding..." : "");
    }
    if (s_ace_rev_shown != g_ace_rev) {
        s_ace_rev_shown = g_ace_rev;
        if (g_ace_heard[0]) snprintf(s_ace_buf, sizeof(s_ace_buf), "\"%s\"\n\n%s", g_ace_heard, g_ace_reply);
        else strlcpy(s_ace_buf, g_ace_reply, sizeof(s_ace_buf));
        lv_label_set_text(s_ace_text, s_ace_buf);
        lv_obj_set_style_text_color(s_ace_text, lv_color_hex(g_ace_state == ACE_ERR ? COL_GREY : COL_WHITE), 0);
        lv_obj_scroll_to_y(lv_obj_get_parent(s_ace_text), 0, LV_ANIM_OFF);
    }
}

/* ======================= Pages ======================= */

void page_open(int kind, lv_obj_t *scr)
{
    page_close();
    static const char *const TITLE[PAGE_COUNT] = { "ACE", "STOPWATCH", "STEPS", "DEVX ALERTS", "SWITCHES", "SETTINGS" };
    s_kind = kind;
    s_root = panel(scr, TITLE[kind]);
    s_list = NULL;
    s_status = NULL;
    s_state = -1;
    switch (kind) {
    case PAGE_ACE:       build_ace(s_root); break;
    case PAGE_STOPWATCH: stopwatch_build(s_root); break;
    case PAGE_STEPS:    build_steps(s_root); break;
    case PAGE_ALERTS:   build_alerts(s_root); break;
    case PAGE_SWITCHES: build_switches(s_root); break;
    case PAGE_SETTINGS: build_settings(s_root); break;
    }
}

void page_close(void)
{
    if (s_kind == PAGE_STOPWATCH) stopwatch_close();
    if (s_root) {
        lv_obj_delete_async(s_root);   /* may be inside one of its own events (a swipe) */
        s_root = NULL;
    }
    s_list = NULL;
    s_status = NULL;
    s_gate_btn = NULL;
    s_ace_btn = NULL;
    g_ace_hold = false;
    s_kind = -1;
}

void page_tick(void)
{
    switch (s_kind) {
    case PAGE_ALERTS:   tick_alerts(); break;
    case PAGE_SWITCHES: tick_switches(); break;
    case PAGE_ACE:      tick_ace(); break;
    default: break;
    }
}
