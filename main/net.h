#pragma once
#include <stdbool.h>

/* HUD networking: the Wi-Fi networks the watch saved (home, office, gym...) are kept.
   At start-up and every 30 min: join the strongest one, set the clock, check for an
   update, then turn Wi-Fi off again. */
void net_start(void);
void net_sync_now(void);
const char *net_status(void);   /* short text for the set-up screen */
