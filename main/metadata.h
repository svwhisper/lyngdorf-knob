#pragma once

#include "esp_err.h"

// Spawns a background task that polls the amp's HTTP/JSON API
// (http://<amp_ip>:8080/api/getData) for now-playing metadata and updates
// g_state. Poll interval is configurable via the web UI (NVS_META_POLL_S),
// default DEFAULT_META_POLL_S seconds. No-op if no amp IP is configured.
esp_err_t metadata_init(void);

// Toggle play/pause on the amp via the same HTTP API
// (POST-style call uses GET in this API; "pause" value is a toggle, the
// amp picks the right direction from its current state).
esp_err_t metadata_play_pause(void);

// Skip to next / previous track. Same endpoint as play/pause, different
// JSON value. Per docs/references/lyngdorf-api.md the streaming module
// accepts {"control":"next"} and {"control":"previous"} on the
// player:player/control path with the "activate" role.
esp_err_t metadata_next_track(void);
esp_err_t metadata_prev_track(void);
