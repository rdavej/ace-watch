/*
 * Rest mode (see rest.h).
 *
 * Asleep, the watch uses almost nothing: the chip is in deep sleep, the screen and
 * touch chip are put to sleep, the speaker amp is held off. Every REST_PEEK_S seconds
 * the chip wakes for a moment, reads the motion sensor directly (no screen, no Wi-Fi)
 * and goes back to sleep unless the watch was moved or the charger plugged in.
 */
#include "rest.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "rest";

#define REST_MAGIC   0x5E57AC31u
#define REST_PEEK_S  6            /* how often to check for movement while resting */
#define MOVED_DOT    0.985f       /* tilted more than ~10 degrees = picked up */
#define MOVED_MG     150.0f       /* or shaken */

#define IMU_ADDR     0x6B         /* QMI8658 */
#define IMU_AX_L     0x35
#define PMU_ADDR     0x34         /* AXP2101 */
#define TP_ADDR      0x38         /* FT3168 touch */
#define PA_GPIO      GPIO_NUM_6   /* speaker amp */
#define LCD_RST_GPIO GPIO_NUM_11

typedef struct {
    uint32_t magic;
    float g[3];                   /* which way gravity pointed when it went to rest */
    uint32_t peeks;
    int32_t bat;                  /* battery % when it went to rest */
} rest_rtc_t;

static RTC_DATA_ATTR rest_rtc_t s_rest;
static int s_last_minutes = 0, s_last_bat = -1;   /* the rest that just ended */

static void sleep_now(void)
{
    esp_sleep_enable_timer_wakeup((uint64_t)REST_PEEK_S * 1000000ULL);
    esp_deep_sleep_start();
}

/* Read the motion sensor and charger with a throw-away I2C bus (the full start-up
   makes its own one later). Returns true if the watch should wake up. */
static bool should_wake(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = 0,
        .sda_io_num = GPIO_NUM_8,
        .scl_io_num = GPIO_NUM_7,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return true;   /* can't tell: wake */
    bool wake = true;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = IMU_ADDR, .scl_speed_hz = 400000 };
    i2c_master_dev_handle_t imu = NULL, pmu = NULL;
    if (i2c_master_bus_add_device(bus, &dc, &imu) == ESP_OK) {
        uint8_t reg = IMU_AX_L, d[6];
        if (i2c_master_transmit_receive(imu, &reg, 1, d, 6, 30) == ESP_OK) {
            float a[3];
            for (int i = 0; i < 3; i++) a[i] = (int16_t)(d[2 * i] | (d[2 * i + 1] << 8)) * (1000.0f / 8192.0f);  /* +-4 g */
            float m = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            float dot = m > 1.0f ? (a[0] * s_rest.g[0] + a[1] * s_rest.g[1] + a[2] * s_rest.g[2]) / m : 0;
            wake = dot < MOVED_DOT || fabsf(m - 1000.0f) > MOVED_MG;
        }
    }
    dc.device_address = PMU_ADDR;
    if (!wake && i2c_master_bus_add_device(bus, &dc, &pmu) == ESP_OK) {
        uint8_t reg = 0x00, st1 = 0;
        if (i2c_master_transmit_receive(pmu, &reg, 1, &st1, 1, 30) == ESP_OK && (st1 & 0x20)) wake = true;  /* charger in */
    }
    if (imu) i2c_master_bus_rm_device(imu);
    if (pmu) i2c_master_bus_rm_device(pmu);
    i2c_del_master_bus(bus);
    return wake;
}

bool rest_wake_check(void)
{
    if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER || s_rest.magic != REST_MAGIC) {
        s_rest.magic = 0;
        gpio_hold_dis(PA_GPIO);
        return false;
    }
    if (!should_wake()) {          /* still lying there: back to sleep (the amp stays held off) */
        s_rest.peeks++;
        sleep_now();
    }
    gpio_hold_dis(PA_GPIO);
    ESP_LOGI(TAG, "Picked up after %lu checks (~%lu min resting)",
             (unsigned long)s_rest.peeks, (unsigned long)(s_rest.peeks * REST_PEEK_S / 60));
    s_last_minutes = (int)(s_rest.peeks * REST_PEEK_S / 60);
    s_last_bat = s_rest.bat;
    s_rest.magic = 0;
    return true;
}

int rest_minutes(void) { return s_last_minutes; }
int rest_battery_before(void) { return s_last_bat; }

void rest_enter(const float gravity[3], int battery_pct)
{
    float m = sqrtf(gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2]);
    if (m < 1.0f) return;
    for (int i = 0; i < 3; i++) s_rest.g[i] = gravity[i] / m;
    s_rest.peeks = 0;
    s_rest.bat = battery_pct;
    s_rest.magic = REST_MAGIC;
    ESP_LOGI(TAG, "Lying still: resting until picked up");

    /* touch chip: hibernate (the full start-up resets it awake again) */
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    i2c_master_dev_handle_t tp = NULL;
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TP_ADDR, .scl_speed_hz = 400000 };
    if (bus && i2c_master_bus_add_device(bus, &dc, &tp) == ESP_OK) {
        uint8_t cmd[2] = {0xA5, 0x03};
        i2c_master_transmit(tp, cmd, 2, 30);
        i2c_master_bus_rm_device(tp);
    }
    /* screen: a reset pulse leaves the AMOLED in its sleep state */
    gpio_set_direction(LCD_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST_GPIO, 1);
    /* speaker amp off and held off */
    gpio_set_direction(PA_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(PA_GPIO, 0);
    gpio_hold_en(PA_GPIO);

    vTaskDelay(pdMS_TO_TICKS(20));
    sleep_now();
}
