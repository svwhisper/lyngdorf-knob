#include "log_buffer.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_BUF_SIZE 8192

static char    s_log_buf[LOG_BUF_SIZE];
static size_t  s_head        = 0;     // next byte to write
static bool    s_wrapped     = false;
static portMUX_TYPE s_lock   = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev = NULL;  // underlying UART/USB-CDC vprintf

static int log_hook(const char *fmt, va_list args) {
    // Forward to the original vprintf first so the serial console still
    // works exactly as before. Use va_copy because vprintf consumes args.
    va_list args2;
    va_copy(args2, args);
    int written = (s_prev) ? s_prev(fmt, args) : vprintf(fmt, args);

    char tmp[256];
    int  len = vsnprintf(tmp, sizeof(tmp), fmt, args2);
    va_end(args2);

    if (len > 0) {
        if (len >= (int)sizeof(tmp)) len = sizeof(tmp) - 1;
        portENTER_CRITICAL(&s_lock);
        for (int i = 0; i < len; i++) {
            s_log_buf[s_head++] = tmp[i];
            if (s_head >= LOG_BUF_SIZE) { s_head = 0; s_wrapped = true; }
        }
        portEXIT_CRITICAL(&s_lock);
    }
    return written;
}

void log_buffer_init(void) {
    // Install the hook once. esp_log_set_vprintf returns the previous
    // vprintf-like handler (typically the UART/USB-CDC one) which we keep
    // calling so console output still flows.
    s_prev = esp_log_set_vprintf(log_hook);
}

size_t log_buffer_dump(char *dst, size_t dst_len) {
    if (!dst || dst_len == 0) return 0;

    portENTER_CRITICAL(&s_lock);
    size_t head    = s_head;
    bool   wrapped = s_wrapped;
    portEXIT_CRITICAL(&s_lock);

    size_t out = 0;
    if (wrapped) {
        // Oldest data lives from `head` to end of buffer, then 0..head.
        size_t tail_len = LOG_BUF_SIZE - head;
        if (tail_len > dst_len) tail_len = dst_len;
        memcpy(dst, s_log_buf + head, tail_len);
        out = tail_len;
        if (out < dst_len) {
            size_t head_len = head;
            if (head_len > dst_len - out) head_len = dst_len - out;
            memcpy(dst + out, s_log_buf, head_len);
            out += head_len;
        }
    } else {
        size_t head_len = head;
        if (head_len > dst_len) head_len = dst_len;
        memcpy(dst, s_log_buf, head_len);
        out = head_len;
    }
    return out;
}
