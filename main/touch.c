#include "touch.h"
#include "app_config.h"
#include "power.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "touch";

// CST816D register map
#define CST816_REG_GESTURE  0x01
#define CST816_REG_FINGERS  0x02
#define CST816_REG_XH       0x03
#define CST816_REG_XL       0x04
#define CST816_REG_YH       0x05
#define CST816_REG_YL       0x06

// Double-tap detection thresholds
#define DTAP_MAX_MS         400
#define DTAP_MAX_MOVE_PX    40

// Edge-based tap state: single tap fires only after DTAP_MAX_MS with no follow-up press
static bool    s_prev_down       = false;
static bool    s_pending_single  = false;
static int64_t s_release_time_us = 0;
static uint16_t s_press_x = 0, s_press_y = 0;

static esp_err_t cst816_read(uint8_t reg, uint8_t *buf, size_t len) {
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (TOUCH_I2C_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, buf, len, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(TOUCH_I2C_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    uint8_t buf[4] = {0};
    bool i2c_ok = (cst816_read(CST816_REG_XH, buf, 4) == ESP_OK);
    uint8_t fingers = 0;
    if (i2c_ok) cst816_read(CST816_REG_FINGERS, &fingers, 1);

    bool currently_down = (i2c_ok && fingers > 0);
    uint16_t x = ((buf[0] & 0x0F) << 8) | buf[1];
    uint16_t y = ((buf[2] & 0x0F) << 8) | buf[3];
    int64_t now = esp_timer_get_time();

    // Rising edge: finger just touched down
    if (!s_prev_down && currently_down) {
        power_signal_activity();
        if (s_pending_single) {
            // A previous tap is waiting — check if this is a double-tap
            int64_t gap_ms = (now - s_release_time_us) / 1000;
            uint16_t dx = (x > s_press_x) ? (x - s_press_x) : (s_press_x - x);
            uint16_t dy = (y > s_press_y) ? (y - s_press_y) : (s_press_y - y);
            if (gap_ms < DTAP_MAX_MS && dx < DTAP_MAX_MOVE_PX && dy < DTAP_MAX_MOVE_PX) {
                lk_cmd_t cmd = { .type = CMD_PLAY_PAUSE, .param = 0 };
                xQueueSend(g_cmd_queue, &cmd, 0);
            } else {
                // Gap too long — the previous tap was a standalone single tap
                lk_cmd_t cmd = { .type = CMD_MUTE_TOGGLE, .param = 0 };
                xQueueSend(g_cmd_queue, &cmd, 0);
            }
            s_pending_single = false;
        }
        s_press_x = x;
        s_press_y = y;
    }

    // Falling edge: finger just lifted — arm the single-tap timer
    if (s_prev_down && !currently_down) {
        s_release_time_us = now;
        s_pending_single  = true;
    }

    // Single-tap timeout: no second press arrived within the window
    if (s_pending_single) {
        int64_t gap_ms = (now - s_release_time_us) / 1000;
        if (gap_ms >= DTAP_MAX_MS) {
            lk_cmd_t cmd = { .type = CMD_MUTE_TOGGLE, .param = 0 };
            xQueueSend(g_cmd_queue, &cmd, 0);
            s_pending_single = false;
        }
    }

    s_prev_down   = currently_down;
    data->point.x = x;
    data->point.y = y;
    data->state   = currently_down ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
}

esp_err_t touch_init(void) {
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = TOUCH_SDA_GPIO,
        .scl_io_num       = TOUCH_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = TOUCH_I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(TOUCH_I2C_PORT, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(TOUCH_I2C_PORT, I2C_MODE_MASTER, 0, 0, 0));

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_read_cb;
    lv_indev_drv_register(&indev_drv);

    ESP_LOGI(TAG, "CST816D ready on I2C addr 0x%02X", TOUCH_I2C_ADDR);
    return ESP_OK;
}
