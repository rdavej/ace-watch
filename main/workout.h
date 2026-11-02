#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* Workout screen: rest timer ring, rest presets, set counter, workout clock.
   Swipe left on the watch face to open it, swipe right to go back. */

/* Build the (hidden) workout screen on top of the face */
void workout_build(lv_obj_t *scr);

/* Show / hide the workout screen */
void workout_show(bool show);
bool workout_visible(void);

/* Call every 250 ms. Returns true once when a rest has just finished
   (the caller wakes the screen). */
bool workout_tick(void);

/* Sets done in the current workout (shown on the watch face) */
int workout_sets(void);
