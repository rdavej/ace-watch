#pragma once
#include <stdbool.h>

/* Helmet HUD mode: the board lies face-up in the chin bar under the lens and the
   picture reflects off the visor, so it is drawn flipped. Everything sits inside one
   round area (the part the lens can see); its spot and size are set on the desk. */
void hud_open(void);
void hud_close(void);
bool hud_is_open(void);
