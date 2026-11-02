#pragma once
#include <stdint.h>
#include <stdbool.h>

#define STEP_GOAL 8000

/* Steps counted today (resets at midnight once the time is known) */
extern volatile uint32_t g_steps_today;

/* Set when the wrist is raised; main.c wakes the screen and clears it */
extern volatile bool g_raise_to_wake;

/* Start the motion sensor task (call after the display/I2C is up) */
void motion_start(void);
