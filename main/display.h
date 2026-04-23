#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t display_init(void);
void      display_set_backlight(uint8_t pct);  // 0–100
void      display_sleep(bool sleep);           // panel on/off (backlight unchanged)
