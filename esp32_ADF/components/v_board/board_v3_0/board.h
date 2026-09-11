/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _AUDIO_BOARD_H_
#define _AUDIO_BOARD_H_

#include "audio_hal.h"
#include "board_def.h"
#include "board_pins_config.h"
#include "esp_peripherals.h"
#include "display_service.h"
#include "periph_sdcard.h"


#define RB_SIZE 20*1024


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Audio board handle
 */
struct audio_board_handle {
    audio_hal_handle_t audio_hal; /*!< audio hardware abstract layer handle */
};

typedef struct audio_board_handle *audio_board_handle_t;

/**
 * @brief Initialize audio board
 *
 * @return The audio board handle
 */
audio_board_handle_t audio_board_init(void);

/**
 * @brief Initialize codec chip
 *
 * @return The audio hal handle
 */
audio_hal_handle_t audio_board_codec_init(void);

/**
 * @brief Initialize led peripheral and display service
 *
 * @return The audio display service handle
 */
display_service_handle_t audio_board_led_init(void);

/**
 * @brief Initialize key peripheral
 *
 * @param set The handle of esp_periph_set_handle_t
 *
 * @return
 *     - ESP_OK, success
 *     - Others, fail
 */
esp_err_t audio_board_key_init(esp_periph_set_handle_t set);

/**
 * @brief Deinitialize key peripheral
 *
 * @return
 *     - ESP_OK, success    
 *     - Others, fail
 */
esp_err_t audio_board_key_deinit();

/**
 * @brief Set key press time
 *
 * @param key_id The key id
 * @param long_press_time_ms Long press time in ms
 * @param long_long_press_time_ms Long long press time in ms
 *
 * @return
 *     - ESP_OK, success
 *     - Others, fail
 */
esp_err_t audio_board_key_set_press_time(int key_id, int long_press_time_ms, int long_long_press_time_ms);

/**
 * @brief Block PLAY events after key boot until the key is stably released
 */
void audio_board_play_boot_release_guard_start(void);

/**
 * @brief Initialize sdcard peripheral
 *
 * @param set The handle of esp_periph_set_handle_t
 *
 * @return
 *     - ESP_OK, success
 *     - Others, fail
 */
esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set);

/**
 * @brief Query audio_board_handle
 *
 * @return The audio board handle
 */
audio_board_handle_t audio_board_get_handle(void);

/**
 * @brief Uninitialize the audio board
 *
 * @param audio_board The handle of audio board
 *
 * @return  0       success,
 *          others  fail
 */
esp_err_t audio_board_deinit(audio_board_handle_t audio_board);

esp_err_t audio_board_enable_sdcard_reader();
esp_err_t audio_board_disable_sdcard_reader();
esp_err_t audio_board_sdcard_reader_IO_config();

esp_err_t audio_board_sdcard_unmount();

esp_err_t board_battery_init(esp_periph_set_handle_t set);

esp_err_t audio_board_battery_deinit();


// esp_err_t board_display_set_play_state(uint8_t state);

// esp_err_t board_display_set_volume(uint8_t volume_pct);



// esp_err_t board_display_set_bt_state(uint8_t state);

// esp_err_t board_display_set_charge_state(uint8_t state);

// esp_err_t board_display_set_battery_level(int pct);
// esp_err_t displayer_track_num( uint16_t song_num);
esp_err_t board_shut_down();


//amplifier
esp_err_t board_amplifier_disable();
esp_err_t board_amplifier_anti_distortion_enable();
esp_err_t board_amplifier_normal_enable();


esp_err_t board_oled_init(void);
esp_err_t audio_board_get_user_voice_volume(int *volume);
esp_err_t audio_board_set_runtime_voice_limit(int volume);
void audio_board_enable_volume_ui(void);
void dispayer_battery(int pct, bool outline_only);
void displayer_volume(int pct);
void displayer_track_num(int num);
void displayer_track_clear();
void displayer_text_bt();
void displayer_bt_clear();
void displayer_bt_blink_slow();
void displayer_bt_blink_fast();
void displayer_bt_blink_stop();
void displayer_charge_blink_start();
void displayer_charge_blink_stop();
void displayer_lock_img_show();
/**
 * @brief 函数名称: displayer_poweron
 * @param 无
 * @return 无
 * @note 作用: 向 OLED 发送 0xAF 开屏命令，仅恢复显示输出，不改动当前 framebuffer 内容。
 */
void displayer_poweron();
/**
 * @brief 函数名称: displayer_poweroff
 * @param 无
 * @return 无
 * @note 作用: 向 OLED 发送 0xAE 关屏命令，仅关闭显示输出，不清空当前 framebuffer 内容。
 */
void displayer_poweroff();
/**
 * @brief 函数名称: board_oled_set_auto_sleep_enabled
 * @param enable 是否启用 3 分钟无按键自动息屏，true 启用，false 禁用
 * @return 无
 * @note 作用: 控制板级 OLED 智能息屏功能的总开关；禁用时会停止息屏定时器并清理吞键状态。
 */
void board_oled_set_auto_sleep_enabled(bool enable);

#ifdef __cplusplus
}
#endif

#endif
