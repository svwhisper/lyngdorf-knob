#pragma once

#include <stdbool.h>
#include "esp_err.h"

// `show_splash` = false on wake from deep sleep (we want the UI live as soon
// as possible); true on cold boot.
esp_err_t ui_init(bool show_splash);
void      ui_apply_pending_state(void);  // call from UI task when g_state.dirty
void      ui_show_status(const char *msg);

// Records the WiFi info to display in the bottom status line:
//   wifi-only:                  "as <ip>"
//   no wifi yet:                "Connecting to <ssid>..."
// These are remembered and re-emitted whenever the state changes.
void      ui_set_wifi_info(const char *ssid, const char *ip);
