#pragma once

#include "esp_err.h"

// Initialise ADC1 channel 0 (GPIO 1) for battery voltage sensing on the
// Waveshare ESP32-S3-Knob-Touch-LCD-1.8 hardware (2× voltage divider).
// Spawns a periodic timer that updates g_state.battery_pct every 10 s.
// Non-fatal if the ADC fails — battery_pct just stays -1.
esp_err_t battery_init(void);
