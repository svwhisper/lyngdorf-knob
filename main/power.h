#pragma once

#include "esp_err.h"

esp_err_t power_init(void);

// Call from any context (ISR-safe): records that user activity occurred.
// The actual display wake happens on the next power_tick() call.
void power_signal_activity(void);

// Call from ui_task while holding g_lvgl_mutex: drives dim/sleep state machine.
void power_tick(void);
