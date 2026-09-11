#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"

#define OLED_W 60
#define OLED_H 32

typedef struct oled_ssd1315 oled_t;

typedef struct
{
    i2c_master_dev_handle_t i2c_dev;  /*!< I2C master device handle */
    int rst_gpio_num;
    bool flip_h;
    bool flip_v;
    bool use_charge_pump;
    uint8_t contrast;
    uint8_t col_offset;
} oled_cfg_t;


esp_err_t oled_create(const oled_cfg_t *cfg, oled_t **out);
void oled_destroy(oled_t *d);
esp_err_t oled_power_on(oled_t *d);
esp_err_t oled_power_off(oled_t *d);
esp_err_t oled_set_contrast(oled_t *d, uint8_t val);
esp_err_t oled_invert(oled_t *d, bool invert);
void oled_lock(oled_t *d);
void oled_unlock(oled_t *d);
void oled_clear(oled_t *d);
void oled_update(oled_t *d);
void oled_update_locked(oled_t *d);
void oled_draw_pixel(oled_t *d, int x, int y, bool on);
void oled_draw_hline(oled_t *d, int x, int y, int w, bool on);
void oled_draw_vline(oled_t *d, int x, int y, int h, bool on);
void oled_draw_rect(oled_t *d, int x, int y, int w, int h, bool on);
void oled_fill_rect(oled_t *d, int x, int y, int w, int h, bool on);
void oled_draw_char(oled_t *d, int x, int y, char c);
void oled_draw_text(oled_t *d, int x, int y, const char *s);
void oled_clear_text(oled_t *d, int x, int y, const char *s);
void oled_draw_battery(oled_t *d, int pct, bool outline_only);
void oled_draw_volume_bar(oled_t *d, int pct);
void oled_clear_track_num(oled_t *d, int x0, int y0);
void oled_draw_track_num(oled_t *d, int x0, int y0, int track);
void oled_draw_lock_img(oled_t *d, int x, int y);
void oled_clear_lock_img(oled_t *d, int x, int y);
void oled_lock_img_show_timed(oled_t *d, int img_x, int img_y, int track_x, int track_y, int restore_track, uint16_t duration_ms);
void oled_lock_img_set_restore_track(oled_t *d, int track);
bool oled_lock_img_is_active(oled_t *d);
void oled_draw_bt(oled_t *d, int x, int y);
void oled_clear_bt(oled_t *d, int x, int y);
void oled_bt_blink_start(oled_t *d, int x, int y, uint16_t period_ms);
void oled_bt_blink_stop(oled_t *d, bool en);
void oled_charge_blink_start(oled_t *d, uint16_t period_ms);
void oled_charge_blink_stop(oled_t *d);
void oled_anim_set_tick_ms(oled_t *d, uint16_t tick_ms);
uint8_t *oled_fb(oled_t *d); // 240 bytes
