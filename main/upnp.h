#pragma once

#include "esp_err.h"

esp_err_t upnp_init(void);
void      upnp_poll_metadata(void);
void      upnp_play_pause(void);
