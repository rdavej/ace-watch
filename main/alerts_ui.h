#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* DevX alerts: swipe up from the bottom of the face; swipe down to close */
void alerts_ui_open(int unread);
void alerts_ui_close(void);
bool alerts_ui_is_open(void);
void alerts_ui_tick(void);     /* every 250 ms */
