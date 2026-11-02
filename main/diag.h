#pragma once
#include <stdbool.h>

/* "Black box": remembers what the watch was doing across restarts */

/* Call first thing in app_main. Builds g_diag_boot_msg if the last restart was unexpected. */
void diag_boot(void);

/* Record what the watch is doing right now (short text, e.g. "Wi-Fi") */
void diag_phase(const char *phase);

/* Start the freeze guard: restarts the watch if the screen task stops for > 10 s */
void diag_start_guard(void);

/* Called regularly by the screen task to say "still alive" */
void diag_ui_alive(void);

/* e.g. "Last restart: power dip @ Wi-Fi" (empty if nothing to report) */
extern char g_diag_boot_msg[48];
