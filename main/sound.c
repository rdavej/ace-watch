#include "sound.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "bsp/esp-bsp.h"
#include "settings.h"

static const char *TAG = "sound";

#define RATE        22050   /* same as the board's default I2S setup */
#define CHUNK       220     /* 10 ms of samples */

static QueueHandle_t s_q;
static esp_codec_dev_handle_t s_spk;
static int16_t s_buf[CHUNK];

/* Play a tone (or silence when hz = 0) for ms milliseconds */
static void tone(int hz, int ms)
{
    static const int16_t sine16[16] = {
        0, 4594, 8487, 11089, 12000, 11089, 8487, 4594,
        0, -4594, -8487, -11089, -12000, -11089, -8487, -4594,
    };
    uint32_t phase = 0;
    uint32_t step = hz ? (uint32_t)(((uint64_t)hz * 16 * 65536) / RATE) : 0;  /* 16.16 fixed point */
    int chunks = ms / 10;
    for (int c = 0; c < chunks; c++) {
        for (int i = 0; i < CHUNK; i++) {
            if (!hz) { s_buf[i] = 0; continue; }
            s_buf[i] = sine16[(phase >> 16) & 15];
            phase += step;
        }
        /* soft start/stop: avoids clicks */
        if (hz && (c == 0 || c == chunks - 1)) {
            for (int i = 0; i < CHUNK; i++) {
                int k = c == 0 ? i : CHUNK - 1 - i;
                s_buf[i] = (int16_t)((int32_t)s_buf[i] * k / CHUNK);
            }
        }
        esp_codec_dev_write(s_spk, s_buf, sizeof(s_buf));
    }
}

static void sound_task(void *arg)
{
    sound_t s;
    for (;;) {
        xQueueReceive(s_q, &s, portMAX_DELAY);
        int vol = settings_volume();
        if (vol == 0) continue;                /* muted in Settings */

        if (!s_spk) continue;                  /* speaker didn't start at boot: stay silent */
        esp_codec_dev_set_out_vol(s_spk, vol);
        esp_codec_dev_sample_info_t fs = {
            .sample_rate = RATE,
            .channel = 1,
            .bits_per_sample = 16,
        };
        if (esp_codec_dev_open(s_spk, &fs) != ESP_CODEC_DEV_OK) continue;

        /* keep playing anything else queued while the speaker is on */
        do {
            switch (s) {
            case SOUND_TICK:
                tone(1760, 60);
                break;
            case SOUND_GO:
                for (int i = 0; i < 3; i++) {
                    tone(2205, 150);
                    tone(0, 100);
                }
                tone(2205, 400);
                break;
            case SOUND_NOTIFY:
                tone(1760, 90);
                tone(0, 40);
                tone(2205, 160);
                break;
            case SOUND_GOAL:
                tone(1320, 120);
                tone(1760, 120);
                tone(2205, 250);
                break;
            }
        } while (xQueueReceive(s_q, &s, 0) == pdTRUE);

        tone(0, 30);                  /* let the last samples drain */
        esp_codec_dev_close(s_spk);   /* speaker amp off again (saves battery) */
    }
}

void sound_start(void)
{
    /* Set up the speaker once, at start-up while memory is plentiful (not later, mid-workout
       with Wi-Fi on). If it fails the watch simply has no sound instead of crashing. */
    s_spk = bsp_audio_codec_speaker_init();
    ESP_LOGI(TAG, "Speaker %s", s_spk ? "ready" : "not available - sounds off");
    s_q = xQueueCreate(4, sizeof(sound_t));
    xTaskCreate(sound_task, "sound", 3072, NULL, 4, NULL);
}

void sound_play(sound_t s)
{
    if (s_q) xQueueSend(s_q, &s, 0);
}
