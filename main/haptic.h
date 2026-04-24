#pragma once

#include "esp_err.h"

// Initialise DRV2605 over the shared I2C bus. Non-fatal if chip absent.
esp_err_t haptic_init(void);

// Play one tick effect. No-op if disabled or chip not found.
// Called from UI task (encoder_process_events) — safe for I2C.
void haptic_play(void);
