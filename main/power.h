#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_attr.h"

esp_err_t power_init(void);

// ISR-safe (IRAM_ATTR): records that user activity occurred. Called from the
// encoder GPIO ISR and from touch / haptic paths. The actual wake transition
// happens on the next power_tick().
void IRAM_ATTR power_signal_activity(void);

// Call from ui_task while holding g_lvgl_mutex: drives dim / panel-sleep /
// deep-sleep state machine.
void power_tick(void);

// True when the panel has been put to sleep (idle tier). Used by the
// network polling code to stretch its cadences.
bool power_is_idle(void);
