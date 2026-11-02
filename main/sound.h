#pragma once

/* Short beeps through the watch speaker (ES8311 codec) */
typedef enum {
    SOUND_TICK,   /* short tick: last 3 seconds of rest */
    SOUND_GO,     /* rest over: three beeps */
    SOUND_GOAL,   /* step goal reached: rising chirp */
    SOUND_NOTIFY, /* DevX notification: two-tone ding */
} sound_t;

/* Start the sound task (the speaker itself is only switched on when a sound plays) */
void sound_start(void);

/* Queue a sound; returns immediately */
void sound_play(sound_t s);

/* Microphone (ES7210), for talking to Ace: 16 kHz mono 16-bit */
#include <stdbool.h>
#include <stdint.h>
bool sound_mic_open(void);
int sound_mic_read(int16_t *mono, int frames);   /* blocks ~frames/16 ms; returns frames read */
void sound_mic_close(void);
