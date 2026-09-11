#include "esp_log.h"
#include "audio_embed_tone.h"
/**
 * @brief embed tone url
 */
const char * const embed_tone_url[] = {
    "embed://tone/0_bloop_1.mp3",
    "embed://tone/1_bloop_2.mp3",
    "embed://tone/2_greanpatch.mp3",
    "embed://tone/3_mixkit_click_error.mp3",
    "embed://tone/4_shooting.mp3",
};


/**
 * @brief embed tone corresponding resource information, as a variable of the `embed_flash_stream_set_context` function
 */
embed_tone_t g_embed_tone[] = {
    [0] = {
        .address = bloop_1_mp3,
        .size    = 20062,
        },
    [1] = {
        .address = bloop_2_mp3,
        .size    = 33436,
        },
    [2] = {
        .address = greanpatch_mp3,
        .size    = 53036,
        },
    [3] = {
        .address = mixkit_click_error_mp3,
        .size    = 47076,
        },
    [4] = {
        .address = shooting_mp3,
        .size    = 5015,
        },
};