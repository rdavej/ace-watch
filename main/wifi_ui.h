#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* Wi-Fi picker (pull-down panel -> Wi-Fi): nearby networks, type the password on the watch */
void wifi_ui_open(void);
void wifi_ui_close(void);
bool wifi_ui_is_open(void);
bool wifi_ui_busy(void);      /* scanning or joining: keep the screen on */
void wifi_ui_tick(void);      /* every 250 ms */
