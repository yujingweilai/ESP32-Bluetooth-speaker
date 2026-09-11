#ifndef _PERIPH_KEY_H_
#define _PERIPH_KEY_H_

#include "esp_peripherals.h"

typedef enum {
    PERIPH_BUTTON_UNCHANGE = 0, /*!< No event */
    PERIPH_KEY_SHORT_PRESSED,      /*!< When button is pressed */
    PERIPH_KEY_SHORT_RELEASE,      /*!< When button is released */
    PERIPH_KEY_LONG_PRESS, /*!< When button is pressed and kept for more than `long_press_time_ms` */
    PERIPH_KEY_LONG_RELEASE, /*!< When button is released and event PERIPH_BUTTON_LONG_PRESSED happened */
    PERIPH_KEY_LONG_LONG_PRESS,
    PERIPH_KEY_LONG_LONG_RELEASE,
    PERIPH_KEY_REPEAT,
} periph_key_event_id_t;

typedef struct
{
    int long_press_time_ms;
    int long_long_press_time_ms;
    int repeat_time_ms;
    int key_num;
    uint64_t (*key_mask_get)(void);
}periph_key_cfg_t;
esp_periph_handle_t periph_key_init(periph_key_cfg_t *config);
esp_err_t periph_key_set_press_time(esp_periph_handle_t periph, int key_id, int long_press_time_ms, int long_long_press_time_ms);
#define PERIPH_ID_KEY         (AUDIO_ELEMENT_TYPE_PERIPH + 30)
#endif // _PERIPH_KEY_H_