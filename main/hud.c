/*
 * Helmet HUD mode (v0.8.0)
 *
 * The board lies face-up in the helmet's chin bar. A loupe lens sits over it and the
 * picture reflects off the visor, so the whole screen is drawn flipped (up-down by
 * default). Only a round area under the lens is used: black everywhere else, so the
 * AMOLED stays dark and nothing glows outside what the lens shows.
 *
 * Desk test controls:
 *   tap            next screen: Dash -> Next turn -> Alert -> Call -> Start-up
 *   hold 3 s       set-up: move the circle, change its size, change the flip, exit
 *
 * Speed, heading, turns and calls are demo data for now; the Atlas Ride app on the
 * phone sends the real ones later.
 */
#include "hud.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "lvgl_private.h"   /* flush / touch hooks for the flip */
#include "settings.h"

LV_FONT_DECLARE(font_time_86);
int watch_battery(void);

#define COL_ACCENT  0x5AB0FF   /* Atlas blue */
#define COL_WHITE   0xE9EEF8
#define COL_GREY    0x8C9AB8
#define COL_RING    0x1C3B66
#define COL_TRACK   0x0B1830
#define COL_YELLOW  0xFFD23F
#define COL_WARN    0xFF9F43
#define COL_CALL    0x3DDC97

enum { V_DASH, V_TURN, V_ALERT, V_CALL, V_BOOT, V_N };

static lv_obj_t *s_root;        /* full screen, black */
static lv_obj_t *s_dial;        /* the round area */
static lv_obj_t *s_setup;       /* set-up controls (only while setting up) */
static lv_obj_t *s_setup_info;
static lv_timer_t *s_timer;
static int s_view = V_BOOT;
static uint32_t s_view_t0;
static int s_saved_bright = -1;

/* dial parts */
static lv_obj_t *s_ring, *s_ticks, *s_speed_arc;
static lv_obj_t *s_orb, *s_orb_hi;
static lv_obj_t *s_dash, *s_speed, *s_unit, *s_head, *s_turn_chip, *s_turn_txt, *s_info, *s_song, *s_lean;
static lv_obj_t *s_mid, *s_mid_icon, *s_mid_title, *s_mid_big, *s_mid_sub, *s_mid_hint, *s_arrow;
static lv_obj_t *s_small;       /* small speed at the bottom on turn / alert / call */
static lv_obj_t *s_boot, *s_boot_arc, *s_boot_orb, *s_boot_title, *s_boot_sub;

/* ---------- Flip: mirror what's sent to the screen, and the touches ---------- */
static uint8_t s_flip;                     /* bit 0 up-down, bit 1 left-right */
static lv_display_flush_cb_t s_orig_flush;
static lv_indev_read_cb_t s_orig_read;
static lv_display_t *s_disp;
static lv_indev_t *s_touch;
static uint16_t s_row[512];

static void flip_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    if (!s_flip || lv_color_format_get_size(lv_display_get_color_format(d)) != 2) {
        s_orig_flush(d, a, px);
        return;
    }
    int32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
    int32_t W = lv_display_get_horizontal_resolution(d), H = lv_display_get_vertical_resolution(d);
    uint16_t *p = (uint16_t *)px;
    if ((s_flip & 2) && w <= 512) {
        for (int32_t y = 0; y < h; y++) {
            uint16_t *r = p + y * w;
            for (int32_t i = 0, j = w - 1; i < j; i++, j--) { uint16_t t = r[i]; r[i] = r[j]; r[j] = t; }
        }
    }
    if ((s_flip & 1) && w <= 512) {
        for (int32_t i = 0, j = h - 1; i < j; i++, j--) {
            memcpy(s_row, p + i * w, w * 2);
            memcpy(p + i * w, p + j * w, w * 2);
            memcpy(p + j * w, s_row, w * 2);
        }
    }
    lv_area_t b = *a;
    if (s_flip & 1) { b.y1 = H - 1 - a->y2; b.y2 = H - 1 - a->y1; }
    if (s_flip & 2) { b.x1 = W - 1 - a->x2; b.x2 = W - 1 - a->x1; }
    s_orig_flush(d, &b, px);
}

static void flip_read(lv_indev_t *in, lv_indev_data_t *data)
{
    s_orig_read(in, data);
    if (!s_flip || !s_disp) return;
    if (s_flip & 1) data->point.y = lv_display_get_vertical_resolution(s_disp) - 1 - data->point.y;
    if (s_flip & 2) data->point.x = lv_display_get_horizontal_resolution(s_disp) - 1 - data->point.x;
}

static void flip_hooks(void)
{
    if (!s_disp) {
        s_disp = lv_display_get_default();
        if (s_disp && s_disp->flush_cb && s_disp->flush_cb != flip_flush) {
            s_orig_flush = s_disp->flush_cb;
            s_disp->flush_cb = flip_flush;
        }
    }
    if (!s_touch) {
        s_touch = bsp_display_get_input_dev();
        if (s_touch && s_touch->read_cb && s_touch->read_cb != flip_read) {
            s_orig_read = s_touch->read_cb;
            s_touch->read_cb = flip_read;
        }
    }
}

static void set_flip(uint8_t f)
{
    s_flip = s_orig_flush ? (f & 3) : 0;
    lv_obj_invalidate(lv_screen_active());
    lv_obj_invalidate(lv_layer_top());
}

/* ---------- Helpers ---------- */
static int R(void) { return g_set.hud_r; }
static int K(int v) { return v * R() / 120; }   /* sizes are designed for a 240 px circle */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *f, uint32_t col, const char *t)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, t);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *arc(lv_obj_t *parent, int size, int width, uint32_t col, uint32_t bg)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(a, bg ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, lv_color_hex(col), LV_PART_INDICATOR);
    return a;
}

/* The Atlas orb: a blue sphere with a soft glow and a highlight */
static lv_obj_t *orb(lv_obj_t *parent, int d, lv_obj_t **hi)
{
    lv_obj_t *o = box(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x1F6FD6), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(0x9FE0FF), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_color(o, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_shadow_width(o, d, 0);
    lv_obj_set_style_shadow_spread(o, 1, 0);
    lv_obj_set_style_shadow_opa(o, LV_OPA_70, 0);
    lv_obj_t *h = box(o);
    lv_obj_set_size(h, d / 3 > 2 ? d / 3 : 2, d / 3 > 2 ? d / 3 : 2);
    lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_80, 0);
    lv_obj_set_style_bg_color(h, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(h, LV_ALIGN_TOP_LEFT, d / 5, d / 6);
    if (hi) *hi = h;
    return o;
}

/* Turn arrow (right turn): up, then right with a head */
static lv_point_precise_t s_arrow_pts[5];
static lv_obj_t *turn_arrow(lv_obj_t *parent, int s, uint32_t col, int width)
{
    s_arrow_pts[0] = (lv_point_precise_t){ s * 3 / 10, s };
    s_arrow_pts[1] = (lv_point_precise_t){ s * 3 / 10, s * 4 / 10 };
    s_arrow_pts[2] = (lv_point_precise_t){ s * 9 / 10, s * 4 / 10 };
    s_arrow_pts[3] = (lv_point_precise_t){ s * 6 / 10, s * 1 / 10 };
    s_arrow_pts[4] = (lv_point_precise_t){ s * 9 / 10, s * 4 / 10 };
    lv_obj_t *l = lv_line_create(parent);
    lv_line_set_points(l, s_arrow_pts, 5);
    lv_obj_set_size(l, s, s + width);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(col), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void ring_color(uint32_t col)
{
    lv_obj_set_style_arc_color(s_ring, lv_color_hex(col), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_speed_arc, lv_color_hex(col), LV_PART_INDICATOR);
}

/* ---------- Build the round area ---------- */
static void build_dial(void)
{
    if (s_dial) lv_obj_delete(s_dial);
    int r = R(), big = r >= 105;
    s_dial = box(s_root);
    lv_obj_set_size(s_dial, 2 * r, 2 * r);
    lv_obj_align(s_dial, LV_ALIGN_CENTER, g_set.hud_dx, g_set.hud_dy);

    s_ring = arc(s_dial, 2 * r, 2, COL_RING, 0);
    lv_arc_set_bg_angles(s_ring, 0, 360);
    lv_arc_set_angles(s_ring, 0, 360);

    s_ticks = lv_scale_create(s_dial);
    lv_obj_set_size(s_ticks, 2 * r - K(12), 2 * r - K(12));
    lv_obj_center(s_ticks);
    lv_obj_remove_flag(s_ticks, LV_OBJ_FLAG_CLICKABLE);
    lv_scale_set_mode(s_ticks, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_angle_range(s_ticks, 270);
    lv_scale_set_rotation(s_ticks, 135);
    lv_scale_set_total_tick_count(s_ticks, 41);
    lv_scale_set_major_tick_every(s_ticks, 10);
    lv_scale_set_label_show(s_ticks, false);
    lv_obj_set_style_arc_width(s_ticks, 0, LV_PART_MAIN);
    lv_obj_set_style_length(s_ticks, K(4), LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_ticks, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_ticks, lv_color_hex(0x24406E), LV_PART_ITEMS);
    lv_obj_set_style_length(s_ticks, K(9), LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_ticks, 3, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_ticks, lv_color_hex(COL_ACCENT), LV_PART_INDICATOR);

    s_speed_arc = arc(s_dial, 2 * r - K(36), K(6) > 3 ? K(6) : 3, COL_ACCENT, COL_TRACK);
    lv_obj_set_style_arc_rounded(s_speed_arc, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_speed_arc, 135, 45);
    lv_arc_set_range(s_speed_arc, 0, 160);
    lv_arc_set_value(s_speed_arc, 0);

    s_orb = orb(s_dial, K(18), &s_orb_hi);
    lv_obj_align(s_orb, LV_ALIGN_CENTER, -r + K(44), 0);

    /* Dash: big speed */
    s_dash = box(s_dial);
    lv_obj_set_size(s_dash, 2 * r, 2 * r);
    s_head = text(s_dash, &lv_font_montserrat_20, COL_ACCENT, "NE");
    lv_obj_align(s_head, LV_ALIGN_TOP_MID, 0, K(36));
    s_speed = text(s_dash, big ? &font_time_86 : &lv_font_montserrat_48, COL_WHITE, "0");
    lv_obj_align(s_speed, LV_ALIGN_CENTER, 0, -K(8));
    s_unit = text(s_dash, &lv_font_montserrat_16, COL_GREY, "KM/H");
    lv_obj_set_style_text_letter_space(s_unit, 3, 0);
    lv_obj_align(s_unit, LV_ALIGN_CENTER, 0, K(36));
    s_lean = text(s_dash, &lv_font_montserrat_16, COL_GREY, "0\xC2\xB0");
    lv_obj_align(s_lean, LV_ALIGN_CENTER, r - K(40), 0);
    s_song = text(s_dash, &lv_font_montserrat_16, COL_ACCENT, "");
    lv_label_set_long_mode(s_song, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(s_song, K(150));
    lv_obj_align(s_song, LV_ALIGN_CENTER, 0, K(58));
    s_turn_chip = turn_arrow(s_dash, K(18), COL_YELLOW, 3);
    lv_obj_align(s_turn_chip, LV_ALIGN_BOTTOM_MID, -K(30), -K(28));
    s_turn_txt = text(s_dash, &lv_font_montserrat_20, COL_YELLOW, "200 m");
    lv_obj_align(s_turn_txt, LV_ALIGN_BOTTOM_MID, K(12), -K(28));
    s_info = text(s_dash, &lv_font_montserrat_16, COL_GREY, "");
    lv_obj_align(s_info, LV_ALIGN_BOTTOM_MID, 0, -K(6));

    /* Middle card: turn / alert / call */
    s_mid = box(s_dial);
    lv_obj_set_size(s_mid, 2 * r, 2 * r);
    s_arrow = turn_arrow(s_mid, K(56), COL_YELLOW, K(9) > 4 ? K(9) : 4);
    lv_obj_align(s_arrow, LV_ALIGN_CENTER, 0, -K(46));
    s_mid_icon = box(s_mid);
    lv_obj_set_size(s_mid_icon, K(40), K(40));
    lv_obj_set_style_radius(s_mid_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_mid_icon, 3, 0);
    lv_obj_align(s_mid_icon, LV_ALIGN_CENTER, 0, -K(56));
    lv_obj_t *ic = text(s_mid_icon, &lv_font_montserrat_24, COL_WHITE, "!");
    lv_obj_center(ic);
    s_mid_title = text(s_mid, &lv_font_montserrat_16, COL_WARN, "");
    lv_obj_set_style_text_letter_space(s_mid_title, 2, 0);
    lv_obj_align(s_mid_title, LV_ALIGN_CENTER, 0, -K(22));
    s_mid_big = text(s_mid, &lv_font_montserrat_24, COL_WHITE, "");
    lv_obj_align(s_mid_big, LV_ALIGN_CENTER, 0, K(6));
    s_mid_sub = text(s_mid, &lv_font_montserrat_16, COL_GREY, "");
    lv_obj_set_width(s_mid_sub, K(170));
    lv_label_set_long_mode(s_mid_sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s_mid_sub, LV_ALIGN_CENTER, 0, K(34));
    s_mid_hint = text(s_mid, &lv_font_montserrat_16, COL_CALL, "");
    lv_obj_align(s_mid_hint, LV_ALIGN_CENTER, 0, K(56));
    s_small = text(s_mid, &lv_font_montserrat_20, COL_WHITE, "0");
    lv_obj_align(s_small, LV_ALIGN_BOTTOM_MID, 0, -K(14));

    /* Start-up */
    s_boot = box(s_dial);
    lv_obj_set_size(s_boot, 2 * r, 2 * r);
    s_boot_arc = arc(s_boot, 2 * r - K(8), K(4) > 2 ? K(4) : 2, COL_ACCENT, 0);
    lv_obj_set_style_arc_rounded(s_boot_arc, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_boot_arc, 0, 360);
    lv_arc_set_angles(s_boot_arc, 270, 270);
    s_boot_orb = orb(s_boot, K(64), NULL);
    lv_obj_align(s_boot_orb, LV_ALIGN_CENTER, 0, -K(16));
    s_boot_title = text(s_boot, &lv_font_montserrat_20, COL_ACCENT, "LINKING");
    lv_obj_set_style_text_letter_space(s_boot_title, 5, 0);
    lv_obj_align(s_boot_title, LV_ALIGN_CENTER, 0, K(44));
    s_boot_sub = text(s_boot, &lv_font_montserrat_16, COL_GREY, "");
    lv_obj_set_width(s_boot_sub, K(180));
    lv_obj_align(s_boot_sub, LV_ALIGN_CENTER, 0, K(76));

    if (s_setup) {   /* show where the lens circle is while setting up */
        lv_obj_set_style_border_width(s_dial, 1, 0);
        lv_obj_set_style_border_color(s_dial, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_radius(s_dial, LV_RADIUS_CIRCLE, 0);
    }
}

/* ---------- Screens ---------- */
static void hide(lv_obj_t *o, bool h) { if (h) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN); }

static void show_view(int v)
{
    s_view = v;
    s_view_t0 = lv_tick_get();
    bool boot = v == V_BOOT;
    hide(s_boot, !boot);
    hide(s_dash, v != V_DASH);
    hide(s_mid, v == V_DASH || boot);
    hide(s_ring, boot); hide(s_ticks, boot); hide(s_speed_arc, boot); hide(s_orb, boot);
    hide(s_arrow, v != V_TURN);
    hide(s_mid_icon, v != V_ALERT);
    hide(s_mid_hint, v != V_CALL);
    ring_color(v == V_ALERT ? COL_WARN : v == V_CALL ? COL_CALL : COL_ACCENT);
    lv_obj_set_style_text_color(s_mid_title, lv_color_hex(v == V_CALL ? COL_CALL : COL_WARN), 0);
    lv_obj_set_style_border_color(s_mid_icon, lv_color_hex(COL_WARN), 0);

    if (v == V_TURN) {
        lv_label_set_text(s_mid_title, "");
        lv_label_set_text(s_mid_big, "200 m");
        lv_label_set_text(s_mid_sub, "Jln Tebrau");
        lv_obj_align(s_mid_big, LV_ALIGN_CENTER, 0, K(16));
        lv_obj_align(s_mid_sub, LV_ALIGN_CENTER, 0, K(42));
    } else if (v == V_ALERT) {
        lv_label_set_text(s_mid_title, "Rear tyre low");
        lv_label_set_text(s_mid_big, "28 psi");
        lv_label_set_text(s_mid_sub, "Pull over when it's safe");
        lv_obj_align(s_mid_big, LV_ALIGN_CENTER, 0, K(6));
        lv_obj_align(s_mid_sub, LV_ALIGN_CENTER, 0, K(40));
    } else if (v == V_CALL) {
        lv_label_set_text(s_mid_title, "INCOMING CALL");
        lv_label_set_text(s_mid_big, "Caller");
        lv_label_set_text(s_mid_sub, "Mobile");
        lv_label_set_text(s_mid_hint, "Say \"answer\"");
        lv_obj_align(s_mid_title, LV_ALIGN_CENTER, 0, -K(42));
        lv_obj_align(s_mid_big, LV_ALIGN_CENTER, 0, -K(12));
        lv_obj_align(s_mid_sub, LV_ALIGN_CENTER, 0, K(14));
    }
    if (v == V_ALERT) lv_obj_align(s_mid_title, LV_ALIGN_CENTER, 0, -K(20));
    if (boot) {
        lv_arc_set_angles(s_boot_arc, 270, 270);
        lv_obj_set_style_opa(s_boot_orb, LV_OPA_TRANSP, 0);
        lv_label_set_text(s_boot_title, "LINKING");
        lv_label_set_text(s_boot_sub, "");
    }
}

/* Demo ride data: speed swings 35..95, heading turns slowly */
static void demo_tick(uint32_t now)
{
    float t = now / 1000.0f;
    int spd = (int)(65 + 30 * sinf(t / 5.0f) + 4 * sinf(t * 1.3f));
    if (spd < 0) spd = 0;
    static const char *const DIR[8] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int deg = ((int)(t * 6)) % 360;
    char b[32];
    snprintf(b, sizeof(b), "%d", spd);
    lv_label_set_text(s_speed, b);
    lv_label_set_text(s_small, b);
    lv_arc_set_value(s_speed_arc, spd);
    snprintf(b, sizeof(b), "%s %d" "\xC2\xB0", DIR[((deg + 22) / 45) % 8], deg);
    lv_label_set_text(s_head, b);
    int lean = (int)(18 * sinf(t / 3.0f));
    snprintf(b, sizeof(b), "%d" "\xC2\xB0", lean < 0 ? -lean : lean);
    lv_label_set_text(s_lean, b);
    int m = 400 - ((int)(t * 12)) % 400;
    snprintf(b, sizeof(b), "%d m", (m / 10) * 10);
    lv_label_set_text(s_turn_txt, b);

    time_t tt = time(NULL);
    struct tm lt;
    localtime_r(&tt, &lt);
    int bat = watch_battery();
    if (bat >= 0) snprintf(b, sizeof(b), "%d:%02d   \xE2\x80\xA2   %d%%", lt.tm_hour, lt.tm_min, bat);
    else snprintf(b, sizeof(b), "%d:%02d", lt.tm_hour, lt.tm_min);
    lv_label_set_text(s_info, b);
    if (!lv_label_get_text(s_song)[0]) lv_label_set_text(s_song, "Song  \xE2\x80\xA2  Artist  (demo)");

    /* the orb breathes */
    int o = 160 + (int)(95 * (0.5f + 0.5f * sinf(t * 2.2f)));
    lv_obj_set_style_opa(s_orb, o, 0);
}

static void boot_tick(uint32_t el)
{
    if (el < 1000) {                                  /* 1: the ring draws */
        lv_arc_set_angles(s_boot_arc, 270, 270 + el * 360 / 1000);
    } else if (el < 2300) {                           /* 2: Atlas wakes */
        lv_arc_set_angles(s_boot_arc, 0, 360);
        int a = (int)(el - 1000) * 255 / 600;
        lv_obj_set_style_opa(s_boot_orb, a > 255 ? 255 : a, 0);
        lv_label_set_text(s_boot_title, "ATLAS");
    } else if (el < 4300) {                           /* 3: checks, then the dash */
        time_t tt = time(NULL);
        struct tm lt;
        localtime_r(&tt, &lt);
        const char *g = lt.tm_hour < 12 ? "Good morning, boss" : lt.tm_hour < 18 ? "Good afternoon, boss" : "Good evening, boss";
        lv_label_set_text(s_boot_title, g);
        lv_obj_set_style_text_letter_space(s_boot_title, 0, 0);
        lv_label_set_text(s_boot_sub, "Phoenix " LV_SYMBOL_OK "   Asmax " LV_SYMBOL_OK "\nride safe");
    } else {
        lv_obj_set_style_text_letter_space(s_boot_title, 5, 0);
        show_view(V_DASH);
    }
}

static void keep_on_top(void)
{
    lv_obj_t *top = lv_layer_top();
    uint32_t n = lv_obj_get_child_count(top);
    if (n && lv_obj_get_child(top, n - 1) != s_root) lv_obj_move_foreground(s_root);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t now = lv_tick_get();
    keep_on_top();
    if (s_view == V_BOOT) boot_tick(lv_tick_elaps(s_view_t0));
    else demo_tick(now);
}

/* ---------- Set-up (hold 3 s) ---------- */
static void setup_close(bool save);

static void setup_info(void)
{
    if (!s_setup_info) return;
    static const char *const FL[4] = { "No flip", "Up-down", "Left-right", "Both" };
    char b[80];
    snprintf(b, sizeof(b), "%s   size %d   x %+d  y %+d", FL[g_set.hud_flip & 3], 2 * g_set.hud_r, g_set.hud_dx, g_set.hud_dy);
    lv_label_set_text(s_setup_info, b);
}

enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_SMALLER, B_BIGGER, B_FLIP, B_DONE, B_EXIT };

static void setup_btn_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    bool rebuild = false;
    switch (id) {
    case B_UP:    g_set.hud_dy -= 4; break;
    case B_DOWN:  g_set.hud_dy += 4; break;
    case B_LEFT:  g_set.hud_dx -= 4; break;
    case B_RIGHT: g_set.hud_dx += 4; break;
    case B_SMALLER: if (g_set.hud_r > 60) { g_set.hud_r -= 6; rebuild = true; } break;
    case B_BIGGER:  if (g_set.hud_r < 200) { g_set.hud_r += 6; rebuild = true; } break;
    case B_FLIP: {
        static const uint8_t NEXT[4] = { 3, 0, 1, 2 };   /* up-down -> none -> both -> left-right */
        g_set.hud_flip = NEXT[g_set.hud_flip & 3];
        set_flip(g_set.hud_flip);
        break;
    }
    case B_DONE: setup_close(true); return;
    case B_EXIT:
        settings_save();
        hud_close();
        return;
    }
    if (g_set.hud_dx < -200) g_set.hud_dx = -200;
    if (g_set.hud_dx > 200) g_set.hud_dx = 200;
    if (g_set.hud_dy < -250) g_set.hud_dy = -250;
    if (g_set.hud_dy > 250) g_set.hud_dy = 250;
    if (rebuild) { build_dial(); show_view(s_view == V_BOOT ? V_DASH : s_view); }
    else lv_obj_align(s_dial, LV_ALIGN_CENTER, g_set.hud_dx, g_set.hud_dy);
    lv_obj_move_foreground(s_setup);
    setup_info();
}

static void setup_btn(int id, const char *t, int x, int y, int w, int h)
{
    lv_obj_t *b = lv_obj_create(s_setup);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0E2140), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1F4F8F), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(id == B_EXIT ? 0xFF5A5A : COL_ACCENT), 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(b, setup_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)id);
    lv_obj_t *l = text(b, &lv_font_montserrat_20, COL_WHITE, t);
    lv_obj_center(l);
}

static void setup_open(void)
{
    if (s_setup) return;
    s_setup = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_setup);
    lv_obj_set_size(s_setup, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(s_setup, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    setup_btn(B_FLIP, "Flip", 20, 20, 170, 56);
    setup_btn(B_DONE, "Done", 220, 20, 170, 56);
    setup_btn(B_UP, LV_SYMBOL_UP, 173, 90, 64, 56);
    setup_btn(B_LEFT, LV_SYMBOL_LEFT, 8, 223, 56, 64);
    setup_btn(B_RIGHT, LV_SYMBOL_RIGHT, 346, 223, 56, 64);
    setup_btn(B_DOWN, LV_SYMBOL_DOWN, 173, 350, 64, 56);
    setup_btn(B_SMALLER, LV_SYMBOL_MINUS, 20, 418, 110, 56);
    setup_btn(B_BIGGER, LV_SYMBOL_PLUS, 150, 418, 110, 56);
    setup_btn(B_EXIT, "Exit", 280, 418, 110, 56);
    s_setup_info = text(s_setup, &lv_font_montserrat_16, COL_GREY, "");
    lv_obj_align(s_setup_info, LV_ALIGN_BOTTOM_MID, 0, -6);
    setup_info();
    build_dial();                 /* draws the circle outline */
    show_view(s_view == V_BOOT ? V_DASH : s_view);
    lv_obj_move_foreground(s_setup);
}

static void setup_close(bool save)
{
    if (!s_setup) return;
    lv_obj_delete_async(s_setup);   /* closing from one of its own buttons */
    s_setup = NULL;
    s_setup_info = NULL;
    if (save) settings_save();
    build_dial();                 /* without the outline */
    show_view(s_view == V_BOOT ? V_DASH : s_view);
}

/* ---------- Touch on the HUD ---------- */
static void root_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (s_setup) return;
    if (c == LV_EVENT_SHORT_CLICKED) show_view((s_view + 1) % V_N);
    else if (c == LV_EVENT_LONG_PRESSED) setup_open();
}

/* ---------- Open / close ---------- */
void hud_open(void)
{
    if (s_root) return;
    flip_hooks();
    s_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_root, root_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(s_root, root_cb, LV_EVENT_LONG_PRESSED, NULL);
    build_dial();
    show_view(V_BOOT);
    set_flip(g_set.hud_flip);
    s_saved_bright = settings_brightness();
    bsp_display_brightness_set(100);
    s_timer = lv_timer_create(timer_cb, 40, NULL);
}

void hud_close(void)
{
    if (!s_root) return;
    if (s_timer) { lv_timer_delete(s_timer); s_timer = NULL; }
    set_flip(0);
    lv_obj_delete_async(s_root);   /* may be closing from one of its own buttons */
    s_root = NULL;
    s_dial = NULL;
    s_setup = NULL;
    s_setup_info = NULL;
    bsp_display_brightness_set(s_saved_bright >= 0 ? s_saved_bright : settings_brightness());
}

bool hud_is_open(void) { return s_root != NULL; }
