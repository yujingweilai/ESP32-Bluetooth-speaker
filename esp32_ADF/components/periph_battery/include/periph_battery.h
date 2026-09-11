#ifndef _PERIPH_BATTERY_H_
#define _PERIPH_BATTERY_H_





#include "esp_peripherals.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PERIPH_ID_BATTERY         (AUDIO_ELEMENT_TYPE_PERIPH + 31)

typedef struct {
    int vbat;  //mv
    int vdc;  //mv
    bool ischarging;
    bool charge_active; // 当前处于充电电流阶段
    bool is_full; // 充电IC报告是否已充满
}periph_battery_snap_t;
typedef struct {
    int intr_gpio;
    //get vbat fuction
    esp_err_t (*get_snap)(periph_battery_snap_t*);
} periph_battery_cfg_t;

typedef enum{
    BM_EVENT_UNKNOWN,
    BM_EVENT_TIMER,
    BM_EVENT_CHRG_STATUS_CHANGE,
    BM_EVENT_CHRG_IN,
    BM_EVENT_CHRG_OUT,
    BM_EVENT_CHRG_CMPL,
    BM_EVENT_BAT_LOW,
    BM_EVENT_BAT_LEVEL_CHANGE,    
}PERIPH_BATTERY_event_id_t;
esp_periph_handle_t periph_battery_init(periph_battery_cfg_t *cfg);
uint8_t periph_battery();
#ifdef __cplusplus
}
#endif

#endif // _PERIPH_BATTERY_H_
