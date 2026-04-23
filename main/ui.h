#pragma once

#include "esp_err.h"

esp_err_t ui_init(void);
void      ui_apply_pending_state(void);  // call from UI task when g_state.dirty
void      ui_show_status(const char *msg);
