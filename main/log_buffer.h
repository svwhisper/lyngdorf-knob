#pragma once

#include <stddef.h>

// Hook into ESP_LOGx output: every call also writes into an in-memory ring
// buffer that the web server exposes at GET /log. Call this once at boot,
// before any other logging matters. The original UART / USB-CDC output is
// preserved.
void log_buffer_init(void);

// Copy the buffer contents into `dst` in chronological order, up to
// `dst_len` bytes. Returns the number of bytes written. Safe to call
// from a web-server task while logs are streaming in.
size_t log_buffer_dump(char *dst, size_t dst_len);
