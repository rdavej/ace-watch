#pragma once

/* Short beeps through the watch speaker (ES8311 codec) */
typedef enum {
    SOUND_TICK,   /* short tick: last 3 seconds of rest */
    SOUND_GO,     /* rest over: three beeps */
    SOUND_GOAL,   /* step goal reached: rising chirp */
} sound_t;

/* Start the sound task (the speaker itself is only switched on when a sound plays) */
void sound_start(void);

/* Queue a sound; returns immediately */
void sound_play(sound_t s);
