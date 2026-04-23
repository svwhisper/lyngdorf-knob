#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_manager_init(void);
bool      wifi_manager_is_connected(void);
bool      wifi_manager_is_ap_mode(void);
void      wifi_manager_forget(void);   // erase creds and reboot
