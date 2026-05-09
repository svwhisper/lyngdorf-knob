#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t display_init(void);
void      display_set_backlight(uint8_t pct);  // 0–100
void      display_sleep(bool sleep);           // panel on/off (backlight unchanged)

// Deeper power-saving helpers used both before deep sleep AND on entering
// the panel-sleep tier (where we now also shut the panel down for hours
// while the amp is still playing).
//   - display_enter_low_power:  sends SLPIN  (0x10) — powers down the
//                               SH8601's internal regulator / oscillator.
//                               Caller should already have done display_sleep(true).
//   - display_exit_low_power:   sends SLPOUT (0x11) and waits the
//                               required 120 ms before any draw. Pair with
//                               a subsequent display_sleep(false).
//   - display_backlight_off:    stops LEDC and drives BL low.
//   - display_backlight_resume: re-configures LEDC so display_set_backlight()
//                               works again. Call after display_backlight_off().
void      display_enter_low_power(void);
void      display_exit_low_power(void);
void      display_backlight_off(void);
void      display_backlight_resume(void);
