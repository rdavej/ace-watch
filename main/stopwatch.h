#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"

/* Stopwatch with laps (its own screen; keeps running in the background) */
void stopwatch_build(lv_obj_t *parent);   /* called when its screen opens */
void stopwatch_close(void);               /* called when its screen closes */
bool stopwatch_running(void);
void stopwatch_text(char *out, size_t n); /* "3:21" for the watch face */
