#include <string.h>
// #include <stdio.h>
#include "esp_log.h"
#include "audio_mem.h"
#include "sys/queue.h"
#include "periph_key.h"
#include <sys/time.h>

#define HARD_KEY_NUM	    3	   						/* 实体按键数量 */
#define KEY_COUNT   	 	(HARD_KEY_NUM + 2)	/* 3 个实体按键 + 2 个组合按键槽位 */


static const char* TAG = "PERIPH_KEY";
typedef enum {
    KEY_UNCHANGE = 0,
    KEY_SHORT_PRESSED=1,
    KEY_SHORT_RELEASE=2,
    KEY_LONG_PRESS=3,
    KEY_LONG_RELEASE=4,
    KEY_LONG_LONG_PRESS=5,
    KEY_LONG_LONG_RELEASE=6,
    KEY_REPEAT=7,  
} key_status_t;

typedef struct 
{
    	// uint8_t (*IsPress)(void);
    long long                   last_press_tick;
    long long                   last_repeat_tick;
    bool                        long_pressed;
    bool                        long_long_pressed;  

    int long_press_time_ms;
    int long_long_press_time_ms;
    int repeat_time_ms;
}periph_key_item_t;

typedef struct 
{   
    periph_key_item_t key_list[KEY_COUNT];
    int key_num;
    uint64_t (*key_mask_get)(void);
}periph_key_t;


static long long tick_get()
{
    struct timeval te;
    gettimeofday(&te, NULL);
    long long milliseconds = te.tv_sec * 1000LL + te.tv_usec / 1000;
    return milliseconds;
}
static key_status_t key_get_state(periph_key_item_t *btn_item, bool is_press)
{
    if (btn_item->last_press_tick == 0 && is_press) {
        btn_item->last_press_tick = tick_get();
        btn_item->long_pressed = false;
        return KEY_SHORT_PRESSED;
    }
    if(!is_press && btn_item->last_press_tick && btn_item->long_long_pressed){
        btn_item->long_long_pressed = false;
        btn_item->last_press_tick = 0;
        return KEY_LONG_LONG_RELEASE;
    }
    if (!is_press && btn_item->last_press_tick && btn_item->long_pressed) {
        btn_item->last_press_tick = 0;
        btn_item->long_pressed = false;
        return KEY_LONG_RELEASE;
    }
    else if (!is_press && btn_item->last_press_tick) {
        btn_item->last_press_tick = 0;
        btn_item->long_pressed = false;
        return KEY_SHORT_RELEASE;
    }

    if(btn_item->long_pressed && is_press && tick_get() - btn_item->last_repeat_tick > btn_item->repeat_time_ms){
        btn_item->last_repeat_tick = tick_get();
        return KEY_REPEAT;
    }

    if (btn_item->long_pressed == false && is_press && tick_get() - btn_item->last_press_tick > btn_item->long_press_time_ms) {
        btn_item->long_pressed = true;
        btn_item->last_repeat_tick = tick_get();
        return KEY_LONG_PRESS;
    }

    if ( btn_item->long_long_pressed == false  && is_press && tick_get() - btn_item->last_press_tick > btn_item->long_long_press_time_ms){
        btn_item->long_long_pressed = true;
        return KEY_LONG_LONG_PRESS;
    }
    return KEY_UNCHANGE;
}


static void key_timer_handler(xTimerHandle tmr)
{
    esp_periph_handle_t periph = (esp_periph_handle_t) pvTimerGetTimerID(tmr);
    esp_periph_send_cmd(periph, 0, NULL, 0);
}

static esp_err_t _key_init(esp_periph_handle_t self)
{
    ESP_LOGD(TAG,"PERIPH_KEY init..");
    esp_periph_start_timer(self, 100/portTICK_RATE_MS, key_timer_handler);
    return ESP_OK;
}
static esp_err_t _key_run(esp_periph_handle_t self, audio_event_iface_msg_t *msg){ 
    int i;
    periph_key_t *periph_key = esp_periph_get_data(self);
    uint64_t key_mask = periph_key->key_mask_get();
    // ESP_LOGD(TAG,"key mask: %d ", (int)key_mask);
    for(i=0;i<KEY_COUNT;i++){
        periph_key_item_t *item = &(periph_key->key_list[i]);
        key_status_t state = key_get_state(item, 1UL&(key_mask>>i));
        if(state){
            esp_err_t ret = esp_periph_send_event(self, state, (void *)i, 0);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "KEY_ID: %d, ACTION:%d - send_event FAILED: %d", i, state, ret);
            } else {
                ESP_LOGD(TAG,"KEY_ID: %d, ACTION:%d ", i, state);
            }
        }
    }
    return ESP_OK;
}
static esp_err_t _key_destroy(esp_periph_handle_t self)
{
    periph_key_t *periph_key = esp_periph_get_data(self);
    esp_periph_stop_timer(self);
    audio_free(periph_key);
    return ESP_OK;
}

esp_periph_handle_t periph_key_init(periph_key_cfg_t *config)
{
    uint8_t i;
    esp_periph_handle_t periph = esp_periph_create(PERIPH_ID_KEY, "periph_key");
    periph_key_t *periph_key = audio_calloc(1, sizeof(periph_key_t));
    AUDIO_MEM_CHECK(TAG, periph_key, {
        audio_free(periph);
        return NULL;
    });
    memset(periph_key,0,sizeof(periph_key_t));
    for(i=0;i<KEY_COUNT;i++){
        periph_key->key_list[i].long_press_time_ms = config->long_press_time_ms;
        periph_key->key_list[i].long_long_press_time_ms = config->long_long_press_time_ms;
        periph_key->key_list[i].repeat_time_ms = config->repeat_time_ms;
    }
    periph_key->key_num = config->key_num;
    periph_key->key_mask_get = config->key_mask_get;



    esp_periph_set_data(periph, periph_key);
    esp_periph_set_function(periph, _key_init, _key_run, _key_destroy);
    return periph;
}

esp_err_t periph_key_set_press_time(esp_periph_handle_t periph, int key_id, int long_press_time_ms, int long_long_press_time_ms)
{
    periph_key_t *periph_key = esp_periph_get_data(periph);
    if (key_id >= KEY_COUNT) {
        return ESP_FAIL;
    }
    if (long_press_time_ms > 0) {
        periph_key->key_list[key_id].long_press_time_ms = long_press_time_ms;
    }
    if (long_long_press_time_ms > 0) {
        periph_key->key_list[key_id].long_long_press_time_ms = long_long_press_time_ms;
    }
    return ESP_OK;
}


