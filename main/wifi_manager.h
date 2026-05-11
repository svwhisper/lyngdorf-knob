#pragma once

#include "esp_err.h"
#include <stdbool.h>

esp_err_t wifi_manager_init(void);
bool      wifi_manager_is_connected(void);
bool      wifi_manager_is_ap_mode(void);
void      wifi_manager_forget(void);   // erase creds and reboot

// Copy the current IP address as a NUL-terminated string into buf.
// Returns true if an IP has been obtained (STA + DHCP) or if AP mode is
// running (in which case the AP's IP — 192.168.4.1 — is returned).
// Returns false if neither yet, leaving buf untouched.
#include <stddef.h>
bool      wifi_manager_get_ip_str(char *buf, size_t len);
