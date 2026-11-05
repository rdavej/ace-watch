/*
 * Ace Watch motion: QMI8658 accelerometer
 *  - Step counter: acceleration peaks, counted only once 8 arrive in a steady
 *    walking/running rhythm, evenly spaced, and without sharp jolts
 *    (so combing hair, stretching or waving don't count)
 *  - Raise to wake: learns how you hold your wrist when you look at the watch
 *    (from your taps), then wakes when you lift your wrist into that position
 *  - Today's steps saved every few minutes, reset at midnight, last 7 days kept
 */
#include "motion.h"

#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "bsp/esp-bsp.h"
#ifdef M_PI
#undef M_PI /* qmi8658.h defines its own */
#endif
#include "qmi8658.h"
#include "settings.h"

static const char *TAG = "motion";

#define SAMPLE_MS          40      /* 25 Hz */
#define STEP_THRESH_MG     90.0f   /* peak above the running average (v0.7.8: was 110, missed gentle steps) */
#define STEP_JOLT_MG       900.0f  /* sharper than this = arm flick, not a step */
#define STEP_MIN_MS        280     /* ~3.5 steps/s (running) */
#define STEP_MAX_MS        1500    /* ~0.7 steps/s (slow walk) */
#define STEPS_TO_CONFIRM   5       /* rhythm needed before counting (v0.7.8: was 8, short walks were lost) */
#define SAVE_EVERY_MS      (5 * 60 * 1000)

volatile uint32_t g_steps_today = 0;
volatile bool g_raise_to_wake = false;
volatile bool g_learn_view = false;
volatile bool g_wrist_down = false;
step_day_t g_step_history[STEP_HISTORY_DAYS];

static qmi8658_dev_t s_imu;
static bool s_imu_ok;
static volatile int64_t s_last_move_ms;   /* last time the watch moved at all (rest mode) */
static float s_rest_g[3] = {0, 0, 1000};  /* slow average of gravity */
static int s_day = -1; /* day the steps belong to */

/* Learned "looking at the watch" direction of gravity (unit vector) */
static float s_view[3] = {0, 0, 1};
static int s_view_learned = 0; /* number of taps learned from */

/* ---------- Saving ---------- */

static int today(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) return -1; /* time not set yet */
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_year % 100) * 1000 + t.tm_yday; /* e.g. 26305 */
}

static void motion_load(void)
{
    nvs_handle_t h;
    if (nvs_open("steps", NVS_READONLY, &h) != ESP_OK) return;
    int32_t day = -1;
    uint32_t steps = 0;
    nvs_get_i32(h, "day", &day);
    nvs_get_u32(h, "count", &steps);
    size_t sz = sizeof(g_step_history);
    if (nvs_get_blob(h, "hist", g_step_history, &sz) != ESP_OK) {
        memset(g_step_history, 0, sizeof(g_step_history));
    }
    sz = sizeof(s_view);
    if (nvs_get_blob(h, "view", s_view, &sz) == ESP_OK) {
        int32_t n = 0;
        nvs_get_i32(h, "viewn", &n);
        s_view_learned = n;
    }
    nvs_close(h);

    int d = today();
    if (d < 0 || d == day) { /* same day (or time unknown yet): keep counting */
        g_steps_today = steps;
        s_day = day;
    }
}

static void steps_save(void)
{
    nvs_handle_t h;
    if (nvs_open("steps", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "day", s_day);
    nvs_set_u32(h, "count", g_steps_today);
    nvs_set_blob(h, "hist", g_step_history, sizeof(g_step_history));
    nvs_commit(h);
    nvs_close(h);
}

static void view_save(void)
{
    nvs_handle_t h;
    if (nvs_open("steps", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "view", s_view, sizeof(s_view));
    nvs_set_i32(h, "viewn", s_view_learned);
    nvs_commit(h);
    nvs_close(h);
}

/* Midnight rollover / first time the clock becomes known */
static void steps_check_day(void)
{
    int d = today();
    if (d < 0) return;
    if (s_day < 0) {
        s_day = d; /* steps so far belong to today */
    } else if (d != s_day) {
        ESP_LOGI(TAG, "New day, yesterday: %lu steps", (unsigned long)g_steps_today);
        /* Keep the last 7 days, newest first */
        memmove(&g_step_history[1], &g_step_history[0], sizeof(step_day_t) * (STEP_HISTORY_DAYS - 1));
        g_step_history[0].day = s_day;
        g_step_history[0].steps = g_steps_today;
        g_steps_today = 0;
        s_day = d;
        steps_save();
    }
}

/* ---------- Main loop ---------- */

static float dot3(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void motion_task(void *arg)
{
    /* steps */
    float avg = 1000.0f;        /* running average of |a| (gravity ~1000 mg) */
    float smooth = 1000.0f;     /* lightly smoothed |a| */
    bool above = false;
    float peak = 0;
    int64_t peak_ms = 0;
    int64_t last_step_ms = 0;
    int64_t last_gap = 0;
    int pending = 0;            /* steps waiting for rhythm confirmation */

    /* raise to wake */
    float g[3] = {0, 0, 1000};  /* smoothed gravity */
    int64_t last_away_ms = -100000;
    int view_samples = 0;
    int away_samples = 0;
    int down_samples = 0;
    int still_samples = 0;
    int learn_wait = 0;

    int64_t last_save_ms = esp_timer_get_time() / 1000;
    uint32_t saved_steps = g_steps_today;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        int64_t now_ms = esp_timer_get_time() / 1000;

        float a[3];
        if (qmi8658_read_accel(&s_imu, &a[0], &a[1], &a[2]) != ESP_OK) continue;
        float mag = __builtin_sqrtf(dot3(a, a));

        /* ----- Lying still? (rest mode) ----- */
        float dv[3] = {a[0] - s_rest_g[0], a[1] - s_rest_g[1], a[2] - s_rest_g[2]};
        if (dot3(dv, dv) > 60.0f * 60.0f) s_last_move_ms = now_ms;
        for (int i = 0; i < 3; i++) s_rest_g[i] += 0.05f * (a[i] - s_rest_g[i]);

        /* ----- Steps ----- */
        smooth += 0.5f * (mag - smooth);
        avg += 0.04f * (smooth - avg);
        float d = smooth - avg;

        if (!above && d > STEP_THRESH_MG) {
            above = true;
            peak = d;
            peak_ms = now_ms;
        } else if (above) {
            if (d > peak) peak = d;
            if (d < STEP_THRESH_MG * 0.3f) {
                /* Peak finished: decide if it was a step */
                above = false;
                int64_t gap = peak_ms - last_step_ms;
                bool jolt = peak > STEP_JOLT_MG;
                bool in_pace = gap >= STEP_MIN_MS && gap <= STEP_MAX_MS;
                bool even = last_gap == 0 || (gap * 10 >= last_gap * 5 && gap * 10 <= last_gap * 18);

                if (jolt) {
                    pending = 0;                   /* arm flick: start again */
                    last_gap = 0;
                } else if (gap < STEP_MIN_MS) {
                    /* too soon after the last one: ignore this peak */
                } else if (!in_pace || (pending < STEPS_TO_CONFIRM && !even)) {
                    pending = 1;                   /* rhythm broken: this is step 1 */
                    last_gap = 0;
                    last_step_ms = peak_ms;
                } else {
                    last_gap = gap;
                    last_step_ms = peak_ms;
                    if (pending < STEPS_TO_CONFIRM) {
                        pending++;
                        if (pending == STEPS_TO_CONFIRM) {
                            g_steps_today += STEPS_TO_CONFIRM; /* walking confirmed */
                        }
                    } else {
                        g_steps_today++;
                    }
                }
            }
        }

        /* ----- Raise to wake ----- */
        for (int i = 0; i < 3; i++) g[i] += 0.45f * (a[i] - g[i]);   /* quicker to follow the wrist */
        float gm = __builtin_sqrtf(dot3(g, g));
        if (gm < 1.0f) continue;
        float gn[3] = {g[0] / gm, g[1] / gm, g[2] / gm};
        bool still = mag > 800.0f && mag < 1200.0f;
        still_samples = still ? still_samples + 1 : 0;

        /* closeness to the viewing direction: 1 = exactly, 0 = 90 degrees away */
        float c = s_view_learned ? dot3(gn, s_view) : __builtin_fabsf(gn[2]);

        /* Arm lowered for ~1.5 s: lets main.c turn off a raise-woken screen */
        down_samples = (c < 0.4f) ? down_samples + 1 : 0;
        g_wrist_down = down_samples >= 38;

        /* v0.7.3: between the jumpy v0.5 and the too-strict v0.6: the arm down (> ~60 degrees
           away) for ~0.5 s, up into view within 1.5 s, then held fairly steady for ~0.2 s.
           Can be switched off in the quick panel (driving). */
        /* v0.7.8: easier again (worked ~6 times in 10): arm down ~0.3 s, up within 2 s,
           held for ~0.15 s; the screen still goes off by itself if it was a false wake */
        if (c < 0.55f) {
            view_samples = 0;
            if (++away_samples >= 8) last_away_ms = now_ms;
        } else if (c > 0.75f) {
            away_samples = 0;          /* < ~41 degrees from the viewing position */
            view_samples++;
            if (view_samples == 4 && still_samples >= 2 && g_set.raise &&
                (now_ms - last_away_ms) < 2000 + 4 * SAMPLE_MS) {
                g_raise_to_wake = true;
                motion_on_raise();   /* wake the screen right now, no waiting */
            }
        } else {
            away_samples = 0;
        }

        /* Learn the viewing position after a tap-to-wake */
        if (g_learn_view) {
            g_learn_view = false;
            learn_wait = 50; /* look for a still moment in the next 2 s */
        }
        if (learn_wait > 0) {
            learn_wait--;
            if (still_samples >= 8) {
                learn_wait = 0;
                if (!s_view_learned) {
                    memcpy(s_view, gn, sizeof(s_view));
                } else {
                    for (int i = 0; i < 3; i++) s_view[i] = 0.7f * s_view[i] + 0.3f * gn[i];
                    float vm = __builtin_sqrtf(dot3(s_view, s_view));
                    for (int i = 0; i < 3; i++) s_view[i] /= vm;
                }
                if (s_view_learned < 1000) s_view_learned++;
                view_save();
                ESP_LOGI(TAG, "Learned viewing angle (%d): %.2f %.2f %.2f",
                         s_view_learned, s_view[0], s_view[1], s_view[2]);
            }
        }

        /* ----- Day change + saving ----- */
        if (now_ms - last_save_ms > SAVE_EVERY_MS) {
            last_save_ms = now_ms;
            steps_check_day();
            if (g_steps_today != saved_steps) {
                saved_steps = g_steps_today;
                steps_save();
            }
        }
    }
}

uint32_t motion_still_ms(void)
{
    if (!s_imu_ok) return 0;
    return (uint32_t)(esp_timer_get_time() / 1000 - s_last_move_ms);
}

void motion_gravity(float g[3]) { memcpy(g, s_rest_g, sizeof(s_rest_g)); }

void motion_save_now(void)
{
    steps_check_day();
    steps_save();
}

void motion_start(void)
{
    motion_load();
    s_last_move_ms = esp_timer_get_time() / 1000;

    if (bsp_i2c_init() != ESP_OK ||
        qmi8658_init(&s_imu, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH) != ESP_OK) {
        ESP_LOGE(TAG, "Motion sensor not found, steps disabled");
        return;
    }
    qmi8658_enable_gyro(&s_imu, false);              /* not needed, saves power */
    qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_4G);
    qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_31_25HZ);
    qmi8658_set_accel_unit_mps2(&s_imu, false);      /* readings in mg */

    s_imu_ok = true;
    xTaskCreate(motion_task, "motion", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Motion sensor running, %lu steps today", (unsigned long)g_steps_today);
}
