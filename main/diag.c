#include "diag.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_private/panic_internal.h"
#include "riscv/rvruntime-frames.h"

static const char *TAG = "diag";

#define DIAG_MAGIC 0xACE0D1A6u

/* Survives software restarts, watchdog resets and brownouts (not a full power-off) */
typedef struct {
    uint32_t magic;
    char phase[16];
    uint32_t froze;   /* set by the freeze guard just before it restarts */
    uint32_t crash_magic;
    uint32_t pc, ra, cause;   /* where a crash happened */
    char task[12];            /* which task crashed */
} diag_rtc_t;

static RTC_NOINIT_ATTR diag_rtc_t s_rtc;

char g_diag_boot_msg[48] = "";

#define CRASH_MAGIC 0xC0A5DEADu

static volatile int64_t s_ui_beat_us = 0;

void diag_phase(const char *phase)
{
    strlcpy(s_rtc.phase, phase, sizeof(s_rtc.phase));
    s_rtc.magic = DIAG_MAGIC;
}

void diag_boot(void)
{
    bool valid = s_rtc.magic == DIAG_MAGIC;
    const char *phase = valid ? s_rtc.phase : "?";
    esp_reset_reason_t r = esp_reset_reason();
    const char *why = NULL;

    switch (r) {
    case ESP_RST_BROWNOUT:  why = "power dip"; break;
    case ESP_RST_PANIC:     why = "crash"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       why = "watchdog"; break;
    case ESP_RST_SW:        if (valid && s_rtc.froze) why = "freeze"; break; /* plain SW = update/restart */
    default: break;                                                         /* power on, USB, etc. */
    }

    if (why && r == ESP_RST_PANIC && s_rtc.crash_magic == CRASH_MAGIC) {
        s_rtc.task[sizeof(s_rtc.task) - 1] = 0;
        snprintf(g_diag_boot_msg, sizeof(g_diag_boot_msg), "Crash: %s %08lX %08lX c%lu @%s",
                 s_rtc.task, (unsigned long)s_rtc.pc, (unsigned long)s_rtc.ra,
                 (unsigned long)s_rtc.cause, phase);
        ESP_LOGW(TAG, "%s  ra %08lX cause %lu", g_diag_boot_msg,
                 (unsigned long)s_rtc.ra, (unsigned long)s_rtc.cause);
    } else if (why) {
        snprintf(g_diag_boot_msg, sizeof(g_diag_boot_msg), "Last restart: %s @ %s", why, phase);
        ESP_LOGW(TAG, "%s (reset reason %d)", g_diag_boot_msg, (int)r);
    } else {
        ESP_LOGI(TAG, "Reset reason %d", (int)r);
    }

    s_rtc.froze = 0;
    s_rtc.crash_magic = 0;
    diag_phase("boot");
}

void diag_ui_alive(void)
{
    s_ui_beat_us = esp_timer_get_time();
}

static void guard_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        int64_t beat = s_ui_beat_us;
        if (beat && esp_timer_get_time() - beat > 10 * 1000000LL) {
            /* no logging here: a stuck log output may be the very thing that froze */
            s_rtc.froze = 1;
            s_rtc.magic = DIAG_MAGIC;
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    }
}

void diag_start_guard(void)
{
    diag_ui_alive();
    xTaskCreate(guard_task, "guard", 2048, NULL, 10, NULL);
}

/* Runs when the watch crashes, just before the normal crash handler:
   note which task crashed and where, so it can be shown after the restart.
   (Linked in with -Wl,--wrap=esp_panic_handler, see main/CMakeLists.txt) */
extern void __real_esp_panic_handler(panic_info_t *info);

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    s_rtc.pc = s_rtc.ra = s_rtc.cause = 0;
    if (info && info->frame) {
        const RvExcFrame *f = (const RvExcFrame *)info->frame;
        s_rtc.pc = f->mepc;
        s_rtc.ra = f->ra;
        s_rtc.cause = f->mcause;
    }
    const char *n = pcTaskGetName(NULL);
    int i = 0;
    for (; n && n[i] && i < (int)sizeof(s_rtc.task) - 1; i++) s_rtc.task[i] = n[i];
    s_rtc.task[i] = 0;
    s_rtc.crash_magic = CRASH_MAGIC;
    s_rtc.magic = DIAG_MAGIC;
    __real_esp_panic_handler(info);
}
