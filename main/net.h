#pragma once
#include <stdbool.h>
#include <stdint.h>

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

/* ---------- DevX server: smart switches, gate, notifications ---------- */
typedef enum { REQ_IDLE, REQ_BUSY, REQ_OK, REQ_ERR } req_state_t;

#define SW_MAX 12
typedef struct {
    char id[24];
    char name[29];
    int8_t on;        /* 1 on, 0 off, -1 unknown */
    bool online;
    bool gate;
} sw_t;

#define FEED_MAX 10
typedef struct {
    int64_t at;       /* server time, ms */
    char title[41];
    char body[121];
} feed_t;

/* Read these while holding net_lock() */
extern sw_t g_sw[SW_MAX];
extern volatile int g_sw_n;
extern volatile uint32_t g_sw_rev;          /* changes whenever the list/states change */
extern volatile req_state_t g_sw_state;
extern char g_sw_msg[48];
extern volatile req_state_t g_gate_state;
extern char g_gate_msg[48];
extern feed_t g_feed[FEED_MAX];             /* newest first */
extern volatile int g_feed_n;
extern volatile uint32_t g_feed_rev;
extern volatile int g_feed_new;             /* new since last alert (screen clears it) */
extern volatile req_state_t g_feed_state;

bool net_has_key(void);                     /* watch key entered on the setup page */
void net_switches_refresh(void);
void net_switch_set(const char *id, bool on);
void net_gate_open(void);
void net_feed_refresh(void);
bool net_lock(int wait_ms);
void net_unlock(void);
