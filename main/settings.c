#include "settings.h"
#include "nvs.h"

settings_t g_set = { .bright = 2, .volume = 2, .timeout = 1, .live = 0, .raise = 1, .sync = 1 };

static const uint8_t BRIGHT[4] = {40, 60, 80, 100};
static const uint8_t VOLUME[3] = {0, 55, 85};
static const uint16_t TIMEOUT_S[3] = {10, 20, 30};

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    settings_t s = g_set;              /* settings saved by an older version are shorter */
    size_t n = sizeof(s);
    if (nvs_get_blob(h, "set", &s, &n) == ESP_OK && n >= 3 &&
        s.bright < 4 && s.volume < 3 && s.timeout < 3) {
        if (n < 4) s.live = 0;             /* added in v0.5 */
        if (n < 5) s.raise = 1;            /* added in v0.6 */
        if (n < 6) s.sync = 1;             /* added in v0.7.6 */
        if (n < sizeof(s)) s.live_ssid[0] = 0;
        s.live_ssid[sizeof(s.live_ssid) - 1] = 0;
        if (s.live > 2 || (s.live == 2 && !s.live_ssid[0])) s.live = s.live ? 1 : 0;
        s.raise = s.raise ? 1 : 0;
        if (s.sync > 2) s.sync = 1;
        g_set = s;
    }
    nvs_close(h);
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "set", &g_set, sizeof(g_set));
    nvs_commit(h);
    nvs_close(h);
}

int settings_brightness(void) { return BRIGHT[g_set.bright & 3]; }
int settings_volume(void) { return VOLUME[g_set.volume % 3]; }
uint32_t settings_timeout_ms(void) { return TIMEOUT_S[g_set.timeout % 3] * 1000u; }
uint32_t settings_sync_ms(void) { static const uint8_t M[3] = {5, 10, 30}; return M[g_set.sync % 3] * 60000u; }
