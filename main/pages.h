#pragma once
#include "lvgl.h"

enum { PAGE_ACE, PAGE_STOPWATCH, PAGE_STEPS, PAGE_ALERTS, PAGE_SWITCHES, PAGE_SETTINGS, PAGE_COUNT };

/* Build a screen on top of the watch face (closes any other open one) */
void page_open(int kind, lv_obj_t *scr);
/* Remove it again (frees its memory) */
void page_close(void);
/* Call every 250 ms while the screen is on: refreshes live data */
void page_tick(void);
