#pragma once
#include <stdint.h>
#include <stdbool.h>

#define STEP_GOAL 8000
#define STEP_HISTORY_DAYS 7

typedef struct {
    int32_t day;     /* yy*1000 + day-of-year, 0 = empty */
    uint32_t steps;
} step_day_t;

/* Steps counted today (resets at midnight once the time is known) */
extern volatile uint32_t g_steps_today;

/* Previous 7 days, newest first (for syncing to DevX later) */
extern step_day_t g_step_history[STEP_HISTORY_DAYS];

/* Set when the wrist is raised; main.c wakes the screen and clears it */
extern volatile bool g_raise_to_wake;

/* main.c sets this after a tap-to-wake so the watch learns the viewing angle */
extern volatile bool g_learn_view;

/* True while the arm has been lowered away from the viewing angle for ~1.5 s */
extern volatile bool g_wrist_down;

/* Provided by main.c: called from the motion task the moment a raise is seen */
void motion_on_raise(void);

/* Start the motion sensor task (call after the display/I2C is up) */
void motion_start(void);
