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

#ifndef _AUDIO_BOARD_DEFINITION_H_
#define _AUDIO_BOARD_DEFINITION_H_

#include "driver/touch_pad.h"
#include "hal/adc_types.h"

/**
 * @brief SDcard reader  GPIO
 */
#define SDCARD_READER_EN_GPIO (GPIO_NUM_27)

/**
 * @brief SDCARD Function Definition
 */
#define FUNC_SDCARD_EN (1)
#define SDCARD_OPEN_FILE_NUM_MAX 5
// #define SDCARD_INTR_GPIO          GPIO_NUM_NC
#define SDCARD_INTR_GPIO GPIO_NUM_39

#define ESP_SD_PIN_CLK GPIO_NUM_14
#define ESP_SD_PIN_CMD GPIO_NUM_15
#define ESP_SD_PIN_D0 GPIO_NUM_2
#define ESP_SD_PIN_D1 GPIO_NUM_4
#define ESP_SD_PIN_D2 GPIO_NUM_12
#define ESP_SD_PIN_D3 GPIO_NUM_13

#define BOARD_I2C_NUM I2C_NUM_1
#define BOARD_I2C_SDA GPIO_NUM_19
#define BOARD_I2C_SCL GPIO_NUM_22
#define BOARD_I2C_HZ (100000)

typedef enum
{
    KEY_ID_PREV = 0,
    KEY_ID_NEXT,
    KEY_ID_PLAY,
    KEY_ID_BT,
    KEY_ID_TOUCH_LOCK,  // PLAY + NEXT 组合锁键
} Key_id_t;
/**
 * @brief LTK5302 EN
 */
#define GPIO_AMP_EN GPIO_NUM_32

/**
 * @brief LED Function Definition
 */
#define FUNC_SYS_LEN_EN (0)
#define GREEN_LED_GPIO GPIO_NUM_22

/**
 * @brief Audio Codec Chip Function Definition
 */
#ifdef CONFIG_AUDIO_CODEC_ES8388
#define FUNC_AUDIO_CODEC_EN (1)

#define PA_ENABLE_GPIO (-1) //(GPIO_NUM_21)
#define CODEC_ADC_I2S_PORT (0)
#define CODEC_ADC_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_16BIT
#define CODEC_ADC_SAMPLE_RATE (48000)
#define RECORD_HARDWARE_AEC (false)
#define BOARD_PA_GAIN (10) /* Power amplifier gain defined by board (dB) */
// ES8388
extern audio_hal_func_t AUDIO_CODEC_ES8388_DEFAULT_HANDLE;
#define AUDIO_CODEC_DEFAULT_CONFIG() {         \
    .adc_input = AUDIO_HAL_ADC_INPUT_LINE1,    \
    .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,    \
    .codec_mode = AUDIO_HAL_CODEC_MODE_DECODE, \
    .i2s_iface = {                             \
        .mode = AUDIO_HAL_MODE_SLAVE,          \
        .fmt = AUDIO_HAL_I2S_NORMAL,           \
        .samples = AUDIO_HAL_48K_SAMPLES,      \
        .bits = AUDIO_HAL_BIT_LENGTH_16BITS,   \
    },                                         \
};
#endif
// ES8311
#ifdef CONFIG_AUDIO_CODEC_ES8311
#define FUNC_AUDIO_CODEC_EN (1)
#define ES8311_MCLK_SOURCE (1) /* 0 From MCLK of esp32   1 From BCLK */
#define HEADPHONE_DETECT (-1)
#define PA_ENABLE_GPIO (-1) // GPIO_NUM_21
#define CODEC_ADC_I2S_PORT (0)
#define CODEC_ADC_BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT
#define CODEC_ADC_SAMPLE_RATE (48000)
#define RECORD_HARDWARE_AEC (true)
#define BOARD_PA_GAIN (6) /* Power amplifier gain defined by board (dB) */
extern audio_hal_func_t AUDIO_CODEC_ES8311_DEFAULT_HANDLE;
#define AUDIO_CODEC_DEFAULT_CONFIG() {       \
    .adc_input = AUDIO_HAL_ADC_INPUT_LINE1,  \
    .dac_output = AUDIO_HAL_DAC_OUTPUT_ALL,  \
    .codec_mode = AUDIO_HAL_CODEC_MODE_BOTH, \
    .i2s_iface = {                           \
        .mode = AUDIO_HAL_MODE_SLAVE,        \
        .fmt = AUDIO_HAL_I2S_NORMAL,         \
        .samples = AUDIO_HAL_48K_SAMPLES,    \
        .bits = AUDIO_HAL_BIT_LENGTH_16BITS, \
    },                                       \
};
#endif
// BM pins
// #define BM_ADC_UNIT              (ADC_UNIT_1)
// #define BM_ADC_CHANNEL           (ADC_CHANNEL_6)  //pin 34
#define BM_CHARGE_IN_DETECT_PIN (GPIO_NUM_34)
// #define BM_BAT_DET_ENABLE_PIN    (-1)
// #define BM_BAT_CHARGE_DONE_N_PIN (GPIO_NUM_36)  //not use yet

#endif
