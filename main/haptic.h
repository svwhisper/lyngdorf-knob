#pragma once

#include "esp_err.h"

// Initialise DRV2605 over the shared I2C bus. Non-fatal if chip absent.
esp_err_t haptic_init(void);

// Play one tick effect. No-op if disabled or chip not found.
// Called from UI task (encoder_process_events) — safe for I2C.
void haptic_play(void);

// Put the DRV2605 into standby (REG_MODE bit 6 = 1). Drops its quiescent
// draw from ~1.5 mA to a few µA. Used both before deep sleep and on entry
// to panel-sleep tier.
void haptic_standby(void);

// Bring the DRV2605 back from standby and re-program its sequencer slot.
// Pair with haptic_standby(). Cheap (a few I2C writes).
void haptic_resume(void);
