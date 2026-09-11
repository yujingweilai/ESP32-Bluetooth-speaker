
/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2022 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
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

#ifndef AUDIO_EMBED_TONE_URI_H
#define AUDIO_EMBED_TONE_URI_H

typedef struct {
    const uint8_t * address;
    int size;
} embed_tone_t;

extern const uint8_t bloop_1_mp3[]              asm("_binary_bloop_1_mp3_start");
extern const uint8_t bloop_2_mp3[]              asm("_binary_bloop_2_mp3_start");
extern const uint8_t greanpatch_mp3[]           asm("_binary_greanpatch_mp3_start");
extern const uint8_t mixkit_click_error_mp3[]   asm("_binary_mixkit_click_error_mp3_start");
extern const uint8_t shooting_mp3[]             asm("_binary_shooting_mp3_start");
/**
 * @brief embed tone corresponding resource information, as a variable of the `embed_flash_stream_set_context` function
 */
extern embed_tone_t g_embed_tone[];


/**
 * @brief embed tone url index for `embed_tone_url` array
 */
enum tone_url_e {
    TONE_URL_BLOOP_1 = 0,
    TONE_URL_BLOOP_2,
    TONE_URL_GREANPATCH,
    TONE_URL_MIXKIT_CLICK_ERROR,
    TONE_URL_SHOOTING,
    TONE_URL_MAX,
};


/**
 * @brief embed tone url
 */
extern const char * const embed_tone_url[];

#endif // AUDIO_EMBED_TONE_URI_H