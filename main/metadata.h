#pragma once

#include "esp_err.h"

// Spawns a background task that polls the amp's HTTP/JSON API
// (http://<amp_ip>:8080/api/getData) for now-playing metadata and updates
// g_state. Poll interval is configurable via the web UI (NVS_META_POLL_S),
// default DEFAULT_META_POLL_S seconds. No-op if no amp IP is configured.
esp_err_t metadata_init(void);
