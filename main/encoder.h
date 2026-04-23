#pragma once

#include "esp_err.h"

esp_err_t encoder_init(void);
void      encoder_process_events(void); // call from UI task every loop tick
