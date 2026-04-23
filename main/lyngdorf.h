#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t lyngdorf_init(void);

// Called from net task
void lyngdorf_poll_state(void);
void lyngdorf_vol_delta(int32_t delta_db10);
void lyngdorf_mute_toggle(void);
void lyngdorf_disconnect(void);
