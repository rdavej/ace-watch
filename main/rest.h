#pragma once
#include <stdbool.h>

/*
 * Rest mode: when the watch has been lying still with the screen off for a while
 * (on the table, overnight), it goes into deep sleep and only peeks at the motion
 * sensor every few seconds. Picking it up (or plugging in the charger) wakes it,
 * and it syncs with DevX straight away.
 */

/* First thing in app_main. While resting and nothing moved: goes straight back to
   sleep (never returns). Returns true when the watch just woke up from rest. */
bool rest_wake_check(void);

/* Go to rest now (call with the screen off). Never returns. */
void rest_enter(const float gravity[3], int battery_pct);

/* After a wake from rest: how long it rested and the battery when it went to rest (-1 = unknown) */
int rest_minutes(void);
int rest_battery_before(void);
