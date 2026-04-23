#include "encoder.h"
#include "app_config.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdatomic.h>

static const char *TAG = "encoder";

// Accumulated encoder delta (atomic so timer ISR and UI task can share safely)
static atomic_int s_delta = 0;

// Previous pin state for quadrature decode
static uint8_t s_prev = 0;

// Vol step loaded from NVS
static int32_t s_vol_step = DEFAULT_VOL_STEP;

// ---------------------------------------------------------------------------
// 3 ms polling timer callback — software quadrature decode
// ---------------------------------------------------------------------------
static void encoder_poll_cb(void *arg) {
    uint8_t a = gpio_get_level(ENC_A_GPIO);
    uint8_t b = gpio_get_level(ENC_B_GPIO);
    uint8_t cur = (a << 1) | b;

    // Gray-code state machine: transitions that indicate direction
    static const int8_t table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0,
    };

    int8_t step = table[(s_prev << 2) | cur];
    if (step != 0) {
        atomic_fetch_add(&s_delta, step);
    }
    s_prev = cur;
}

// ---------------------------------------------------------------------------
// Called from UI task — coalesce accumulated ticks and post command
// ---------------------------------------------------------------------------
void encoder_process_events(void) {
    int delta = atomic_exchange(&s_delta, 0);
    if (delta == 0) return;

    lk_cmd_t cmd = {
        .type  = CMD_VOL_CHANGE,
        .param = delta * s_vol_step,   // e.g. 1 detent × 5 = 0.5 dB
    };
    xQueueSend(g_cmd_queue, &cmd, 0);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
esp_err_t encoder_init(void) {
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << ENC_A_GPIO) | (1ULL << ENC_B_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    // Seed state
    s_prev = ((gpio_get_level(ENC_A_GPIO) << 1) | gpio_get_level(ENC_B_GPIO));

    // Load vol step from NVS
    uint32_t step;
    config_get_u32(NVS_VOL_STEP, &step, DEFAULT_VOL_STEP);
    s_vol_step = (int32_t)step;

    // 3 ms polling timer
    const esp_timer_create_args_t args = {
        .callback = encoder_poll_cb,
        .name     = "enc_poll",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 3000));

    ESP_LOGI(TAG, "encoder ready, vol_step=%d (%.1f dB)", s_vol_step, s_vol_step / 10.0f);
    return ESP_OK;
}
