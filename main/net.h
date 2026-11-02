#pragma once

typedef enum {
    NET_IDLE,
    NET_NO_WIFI,     /* no saved networks yet */
    NET_SETUP,       /* setup hotspot running */
    NET_CONNECTING,
    NET_SYNCING,     /* time sync + update check */
    NET_OK,
    NET_OFFLINE,     /* no known network nearby */
    NET_UPDATING,    /* downloading new firmware */
} net_state_t;

extern volatile net_state_t g_net_state;
extern char g_net_last_sync[16];

/* What the sync is doing right now: "Wi-Fi", "Time", "Update" */
extern volatile const char *g_net_phase;

/* Start the background Wi-Fi task (call after nvs_flash_init) */
void net_start(void);

/* Open the "Ace-Watch-Setup" hotspot to add a Wi-Fi network */
void net_request_setup(void);
