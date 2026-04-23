#include "display.h"
#include "app_config.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st77916.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "display";

static esp_lcd_panel_handle_t s_panel = NULL;

// ---------------------------------------------------------------------------
// LVGL flush callback
// ---------------------------------------------------------------------------
static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
    esp_lcd_panel_draw_bitmap(s_panel,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_map);
    lv_disp_flush_ready(drv);
}

// ---------------------------------------------------------------------------
// Backlight via LEDC PWM
// ---------------------------------------------------------------------------
static void backlight_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
}

void display_set_backlight(uint8_t pct) {
    uint32_t duty = (pct * 255) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_sleep(bool sleep) {
    esp_lcd_panel_disp_on_off(s_panel, !sleep);
}

// ---------------------------------------------------------------------------
// LVGL tick source
// ---------------------------------------------------------------------------
static void lvgl_tick_cb(void *arg) {
    lv_tick_inc(2);
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------
esp_err_t display_init(void) {
    // SPI bus (QSPI — four data lines)
    spi_bus_config_t bus = {
        .sclk_io_num     = LCD_CLK_GPIO,
        .data0_io_num    = LCD_D0_GPIO,
        .data1_io_num    = LCD_D1_GPIO,
        .data2_io_num    = LCD_D2_GPIO,
        .data3_io_num    = LCD_D3_GPIO,
        .max_transfer_sz = LCD_H_RES * LCD_V_RES * 2 + 8,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO));

    // Panel IO
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = LCD_CS_GPIO,
        .dc_gpio_num       = -1,
        .spi_mode          = 0,
        .pclk_hz           = LCD_SPI_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 32,
        .lcd_param_bits    = 8,
        .flags.quad_mode   = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io));

    // Panel driver (espressif/esp_lcd_st77916 component)
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = LCD_RST_GPIO,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel  = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    // Backlight
    backlight_init();
    display_set_backlight(80);

    // LVGL init
    lv_init();

    // Draw buffers — 1/10 screen each, DMA-capable internal RAM
    static lv_color_t buf1[LCD_H_RES * (LCD_V_RES / 10)];
    static lv_color_t buf2[LCD_H_RES * (LCD_V_RES / 10)];

    static lv_disp_draw_buf_t draw_buf;
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_H_RES * (LCD_V_RES / 10));

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res    = LCD_H_RES;
    disp_drv.ver_res    = LCD_V_RES;
    disp_drv.flush_cb   = lvgl_flush_cb;
    disp_drv.draw_buf   = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // 2 ms tick timer
    const esp_timer_create_args_t tick_args = {
        .callback        = lvgl_tick_cb,
        .name            = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2000));

    ESP_LOGI(TAG, "ST77916 360x360 ready");
    return ESP_OK;
}
