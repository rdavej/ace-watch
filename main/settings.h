#pragma once
#include <stdint.h>

/* Watch settings (Settings screen), saved in flash */
typedef struct {
    uint8_t bright;    /* 0..3: Low, Mid, High, Max */
    uint8_t volume;    /* 0..2: Off, Low, High */
    uint8_t timeout;   /* 0..2: 10 s, 20 s, 30 s */
    uint8_t live;      /* Live alerts (stay on Wi-Fi, check DevX every minute): 0 off, 1 on, 2 only on live_ssid */
    uint8_t raise;     /* 1 = raise to wake on (turn off while driving) */
    uint8_t sync;      /* normal sync every 0: 5 min, 1: 10 min, 2: 30 min */
    char live_ssid[33];/* "On my hotspot": the network Live alerts are on for */
} settings_t;

extern settings_t g_set;

void settings_load(void);
void settings_save(void);

int settings_brightness(void);      /* 0-100 for the display */
int settings_volume(void);          /* 0-100 for the speaker (0 = muted) */
uint32_t settings_timeout_ms(void);
uint32_t settings_sync_ms(void);      /* time between normal syncs */
