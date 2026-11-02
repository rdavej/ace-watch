/*
 * Ace Watch motion: QMI8658 accelerometer
 *  - Step counter: peaks in the acceleration, only counted when they come in a
 *    walking rhythm (4 in a row), so random arm moves don't count
 *  - Raise to wake: arm hanging -> watch face turned up
 *  - Today's steps are saved to flash every few minutes and reset at midnight
 */
#include "motion.h"

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

static const char *TAG = "motion";

#define SAMPLE_MS          20      /* 50 Hz */
#define STEP_THRESH_MG     110.0f  /* peak above the running average */
#define STEP_MIN_MS        250     /* fastest step (running) */
#define STEP_MAX_MS        2000    /* slower than this = not walking */
#define STEPS_TO_CONFIRM   4       /* rhythm needed before counting */
#define SAVE_EVERY_MS      (5 * 60 * 1000)

volatile uint32_t g_steps_today = 0;
volatile bool g_raise_to_wake = false;

static qmi8658_dev_t s_imu;
static int s_day = -1; /* day-of-year the steps belong to */

/* ---------- Saving ---------- */

static int today(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) return -1; /* time not set yet */
    struct tm t;
    localtime_r(&now, &t);
    return (t.tm_year % 100) * 1000 + t.tm_yday; /* e.g. 26305 */
}

static void steps_load(void)
{
    nvs_handle_t h;
    if (nvs_open("steps", NVS_READONLY, &h) != ESP_OK) return;
    int32_t day = -1;
    uint32_t steps = 0;
    nvs_get_i32(h, "day", &day);
    nvs_get_u32(h, "count", &steps);
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
        g_steps_today = 0;
        s_day = d;
        steps_save();
    }
}

/* ---------- Main loop ---------- */

static void motion_task(void *arg)
{
    float avg = 1000.0f;        /* running average of |a| (gravity ~1000 mg) */
    float smooth = 1000.0f;     /* lightly smoothed |a| */
    bool above = false;
    int64_t last_step_ms = 0;
    int pending = 0;            /* steps waiting for rhythm confirmation */

    /* raise to wake */
    int64_t last_down_ms = -100000;
    int up_samples = 0;

    int64_t last_save_ms = esp_timer_get_time() / 1000;
    uint32_t saved_steps = g_steps_today;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        int64_t now_ms = esp_timer_get_time() / 1000;

        float x, y, z;
        if (qmi8658_read_accel(&s_imu, &x, &y, &z) != ESP_OK) continue;
        float mag = __builtin_sqrtf(x * x + y * y + z * z);

        /* ----- Steps ----- */
        smooth += 0.35f * (mag - smooth);
        avg += 0.02f * (smooth - avg);
        float d = smooth - avg;

        if (!above && d > STEP_THRESH_MG) {
            above = true;
            int64_t gap = now_ms - last_step_ms;
            if (gap >= STEP_MIN_MS) {
                if (gap > STEP_MAX_MS) {
                    pending = 1;               /* rhythm broken: start again */
                } else if (pending < STEPS_TO_CONFIRM) {
                    pending++;
                    if (pending == STEPS_TO_CONFIRM) {
                        g_steps_today += STEPS_TO_CONFIRM; /* walking confirmed */
                    }
                } else {
                    g_steps_today++;
                }
                last_step_ms = now_ms;
            }
        } else if (above && d < STEP_THRESH_MG * 0.3f) {
            above = false;
        }

        /* ----- Raise to wake -----
         * Arm down: gravity mostly sideways (|z| small).
         * Looking at the watch: gravity through the screen (|z| large). */
        float az = __builtin_fabsf(z);
        if (az < 450.0f) {
            last_down_ms = now_ms;
            up_samples = 0;
        } else if (az > 800.0f && mag < 1300.0f) {
            up_samples++;
            /* face held up for ~0.25 s within 1.5 s of the arm being down */
            if (up_samples == 12 && (now_ms - last_down_ms) < 1500) {
                g_raise_to_wake = true;
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

void motion_start(void)
{
    steps_load();

    if (bsp_i2c_init() != ESP_OK ||
        qmi8658_init(&s_imu, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH) != ESP_OK) {
        ESP_LOGE(TAG, "Motion sensor not found, steps disabled");
        return;
    }
    qmi8658_enable_gyro(&s_imu, false);              /* not needed, saves power */
    qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_4G);
    qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_62_5HZ);
    qmi8658_set_accel_unit_mps2(&s_imu, false); /* readings in mg */

    xTaskCreate(motion_task, "motion", 3072, NULL, 3, NULL);
    ESP_LOGI(TAG, "Motion sensor running, %lu steps today", (unsigned long)g_steps_today);
}
