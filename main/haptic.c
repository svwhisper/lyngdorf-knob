#include "haptic.h"
#include "app_config.h"

#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "haptic";

// DRV2605 sits on the same I2C bus as the CST816D touch controller
#define DRV2605_ADDR    0x5A

// Register map
#define REG_MODE        0x01
#define REG_LIBRARY     0x03
#define REG_WAVESEQ0    0x04
#define REG_WAVESEQ1    0x05
#define REG_GO          0x0C
#define REG_FEEDBACK    0x1A    // bit7=1 selects LRA mode

// Effect 7 = "Soft Bump 100%" — crisp, light feel for encoder detents
#define HAPTIC_EFFECT   7

static bool s_ready = false;

static esp_err_t reg_write(uint8_t reg, uint8_t val) {
    i2c_cmd_handle_t h = i2c_cmd_link_create();
    i2c_master_start(h);
    i2c_master_write_byte(h, (DRV2605_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(h, reg, true);
    i2c_master_write_byte(h, val, true);
    i2c_master_stop(h);
    esp_err_t err = i2c_master_cmd_begin(TOUCH_I2C_PORT, h, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(h);
    return err;
}

esp_err_t haptic_init(void) {
    // Wake from standby — this also confirms the chip is present
    if (reg_write(REG_MODE, 0x00) != ESP_OK) {
        ESP_LOGW(TAG, "DRV2605 not found at 0x%02X — haptic unavailable", DRV2605_ADDR);
        return ESP_OK;  // non-fatal: board may not have chip fitted
    }

    reg_write(REG_FEEDBACK, 0xB6);          // LRA mode (bit7=1), default gains
    reg_write(REG_LIBRARY,  0x06);          // LRA waveform library
    reg_write(REG_WAVESEQ0, HAPTIC_EFFECT); // pre-load effect into slot 0
    reg_write(REG_WAVESEQ1, 0x00);          // slot 1 = end of sequence

    s_ready = true;
    ESP_LOGI(TAG, "DRV2605 ready");
    return ESP_OK;
}

void haptic_play(void) {
    if (!s_ready) return;
    reg_write(REG_GO, 0x01);    // effect already in sequencer — just fire
}

void haptic_standby(void) {
    if (!s_ready) return;
    // MODE register: bit 6 = STANDBY. Datasheet quiescent ~1.5 mA active,
    // ~few µA in standby. Don't bother checking error — we're about to
    // cut power anyway.
    reg_write(REG_MODE, 0x40);
    s_ready = false;
}

void haptic_resume(void) {
    if (s_ready) return;
    // Mirror haptic_init: clear standby, re-program LRA mode + library +
    // sequencer slot. If the chip was never present, the first write fails
    // and we silently leave s_ready=false.
    if (reg_write(REG_MODE, 0x00) != ESP_OK) return;
    reg_write(REG_FEEDBACK, 0xB6);
    reg_write(REG_LIBRARY,  0x06);
    reg_write(REG_WAVESEQ0, HAPTIC_EFFECT);
    reg_write(REG_WAVESEQ1, 0x00);
    s_ready = true;
}
