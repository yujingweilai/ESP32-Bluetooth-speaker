#include <stdio.h>
#include <string.h>
#include "periph_battery.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "audio_mem.h"
#include "charger.h"

#include "sys/queue.h"



#define CONFIG_CHARGE_IN_LEVEL      (0)
#define CONFIG_CHARGE_DEBOUNCE          (50)//(50)        //50s
#define CONFIG_DISCHARGE_DEBOUNCE       (20)        //20s
#define CONFIG_CHARGE_FULL_DEBOUNCE_CNT (10)
#define CONFIG_BAT_LOW_VOLTAGE          (3100)
#define CONFIG_BAT_LOW_DEBOUNCE         (50)//mV
#define CONFIG_BAT_LOW_CNT              (20)     

#define BAT_VOL_02P         (3200)       // %10, 3.5V
#define BAT_VOL_25P         (3650)
#define BAT_VOL_50P         (3800)
#define BAT_VOL_75P         (4000)
#define BAT_VOL_100P        (4200)
#define ABS(a)          (a > 0 ? a : -a)

// 添加消息队列句柄
static const char* TAG = "PERIPH_BATTERY";
static QueueHandle_t nvs_write_queue;
#define NVS_NAMESPACE "battery"

typedef struct
{
    int intr_gpio;    
    
    esp_err_t (*get_snap)(periph_battery_snap_t*);

    // uint8_t cap_expect;
    uint8_t low_cnt;
    int voltage;
    uint16_t delay_cnt;
    bool bat_had_low;
    bool is_charge_in;
    uint16_t capacity;
    uint16_t nvs_cap;
    bool batcap_is_init;
    uint8_t full_cnt;
    bool had_charge_active;
    bool full_reported;

}periph_battery_t;

const uint16_t bat_cap_cmp_val[6] = {
    /* 0%, 10%, 25%, 50%, 75%, 100% */
    CONFIG_BAT_LOW_VOLTAGE,
    BAT_VOL_02P,
    BAT_VOL_25P,
    BAT_VOL_50P,
    BAT_VOL_75P,
    BAT_VOL_100P,
};

const uint8_t bat_cap_step_val[6] = {
    0, 1, 2, 25, 50, 75,
};

const uint8_t bat_cap_percent_val[5] = {
    1, 23, 25, 25, 25
};

static void bm_timer_handler(xTimerHandle tmr)
{
    esp_periph_handle_t periph = (esp_periph_handle_t) pvTimerGetTimerID(tmr);
    esp_periph_send_cmd(periph, BM_EVENT_TIMER, NULL, 0);
}





static void save_capacity_to_nvs(uint16_t capacity) {
    // 将数据发送到队列而不是直接写入
    /* 电量 0% 或 100% 时，无视 60 秒限制立即保存 */
    if(capacity != 0 && capacity != 100 && xTaskGetTickCount()< pdMS_TO_TICKS(60000)) return;
    xQueueSend(nvs_write_queue, &capacity, 0);
}

// 添加一个专门处理 NVS 写入的任务
static void nvs_write_task(void *pvParameters) {
    uint16_t capacity;
    while (1) {
        if (xQueueReceive(nvs_write_queue, &capacity, portMAX_DELAY)) {
            // 循环接收，直到队列为空，这样 capacity 中保存的就是最新的值
            while(xQueueReceive(nvs_write_queue, &capacity, 0) == pdPASS);

            nvs_handle_t handle;
            if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
                uint16_t old_cap = 0;
                esp_err_t err = nvs_get_u16(handle, "bat_cap", &old_cap);
                /* 如果读取失败或新旧电量不一致，才写入 NVS 以保护 Flash 寿命 */
                if (err != ESP_OK || old_cap != capacity) {
                    nvs_set_u16(handle, "bat_cap", capacity);
                    nvs_commit(handle);
                    ESP_LOGW(TAG, "NVS save capacity:%d", capacity);
                } else {
                    ESP_LOGD(TAG, "NVS capacity %d unchanged, skip write", capacity);
                }
                nvs_close(handle);
            }
        }
    }
}


static esp_err_t _battery_destroy(esp_periph_handle_t self)
{
    ESP_LOGD(TAG,"on destroy");
    periph_battery_t *periph_battery = esp_periph_get_data(self);
    //save battery capacity
    save_capacity_to_nvs(periph_battery->capacity);

    esp_periph_stop_timer(self);
    audio_free(periph_battery);
    return ESP_OK;
}
static void battery_handle_low( esp_periph_handle_t self )
{
    periph_battery_t *battery = esp_periph_get_data(self);
    if (battery->bat_had_low) {
        if (battery->is_charge_in && (battery->voltage >= CONFIG_BAT_LOW_VOLTAGE + CONFIG_BAT_LOW_DEBOUNCE))
        {
            battery->low_cnt = 0;
            battery->bat_had_low = false;
            // esp_periph_send_cmd(self, BM_EVENT_CHRG_OUT, NULL, 0);
        }   

    } else {
        if (battery->voltage < CONFIG_BAT_LOW_VOLTAGE){
            battery->low_cnt++;
            if(battery->low_cnt >= CONFIG_BAT_LOW_CNT){
                battery->bat_had_low = true;
                esp_periph_send_event(self, BM_EVENT_BAT_LOW, NULL, 0);
                ESP_LOGE(TAG, "battery_had_low, vbat:%d", battery->voltage);
            } 
        }
    }

}
static uint16_t cal_bat_cap(uint8_t bat_cap_step, int vbat){
    uint16_t cap_calculate;
    if (bat_cap_step == 6) {
        cap_calculate = 99;  //只到99，等ic充满后再设置为100  
    } else if (bat_cap_step == 0) {
        cap_calculate = 0;
    } else {
        cap_calculate = (vbat - bat_cap_cmp_val[bat_cap_step - 1]) * bat_cap_percent_val[bat_cap_step - 1];
        cap_calculate /= (bat_cap_cmp_val[bat_cap_step] - bat_cap_cmp_val[bat_cap_step - 1]);
        cap_calculate += bat_cap_step_val[bat_cap_step];
    } 
    return cap_calculate;

}
static void send_bat_cap_event(esp_periph_handle_t self, uint16_t cap_calculate){
    int cap = cap_calculate;
    esp_periph_send_event(self, BM_EVENT_BAT_LEVEL_CHANGE, (void*)cap, 0);
}

static void battery_handle_cap(esp_periph_handle_t self){
    periph_battery_t *battery = esp_periph_get_data(self);
    uint16_t cap_calculate;   
    uint8_t idx;
    bool save_to_nvs = false;
    bool send_cap_event = false;
    for (idx = 0; idx < 6; idx++) {
        if (battery->voltage < bat_cap_cmp_val[idx])
            break;
    }
    cap_calculate = cal_bat_cap(idx, battery->voltage);
    // debug_print("cap_calculate:%d ",cap_calculate);
    if (battery->batcap_is_init) {
        /*
         *If power is dcin OK, batcap can't become higher than before.
         *If power is dcin not OK, batcap can't become lower than before.
         */
		battery->delay_cnt++;
        if (battery->is_charge_in) {
            if (cap_calculate <= battery->capacity) {
                battery->delay_cnt = 0;
            } else {
                /*batcap plus 1 percent every one minute till capacity == cap_calculate*/
                if (battery->delay_cnt == CONFIG_CHARGE_DEBOUNCE) {
                    battery->delay_cnt = 0;
                    if (battery->capacity < 99) {
                        battery->capacity++;
                    }
                    // if(battery->capacity  == 0){
                        save_to_nvs = true;
                    // }
                    send_cap_event = true;
                    ESP_LOGW(TAG, "battery voltage: %d", battery->voltage);
                } 
            }
        } else {
            if (cap_calculate >= battery->capacity) {
                battery->delay_cnt = 0;
            } else {
                /* 动态放电防抖：根据电量区间调整防抖时间，防止开机静置时因电压虚假回落而快速掉电 */
                uint16_t current_debounce = CONFIG_DISCHARGE_DEBOUNCE;
                if (battery->capacity == 100) {
                    current_debounce = 300; /* 100% 保持 5 分钟 (300s)，覆盖常见的静置自动关机时间 */
                } else if (battery->capacity > 90) {
                    current_debounce = 40;  /* 90%-99% 保持较长时间，每 40s 掉 1% */
                } else if (battery->capacity > 80) {
                    current_debounce = 30;  /* 80%-90% 每 30s 掉 1% */
                }
                
                /*batcap sub 1 percent every debounce time till capacity == cap_calculate*/
                if (battery->delay_cnt >= current_debounce) {
                    battery->delay_cnt = 0;
                    battery->capacity--;
                    // if(battery->capacity %5 == 0){
                        save_to_nvs = true;
                    // }
                    send_cap_event = true;
                    ESP_LOGW(TAG, "battery voltage: %d, capacity drop to %d", battery->voltage, battery->capacity);
                } 
            }
        }
    }
    else {

        /* 开机 NVS 信任度：静置时电压回落可能导致计算电量比 NVS 记录值低 20%~25% */
        if( (battery->nvs_cap>=0 && battery->nvs_cap <= 100 ) ) {
            // 如果在充电，或者计算电量在 NVS 容量的 -30% 到 +15% 之间，则信任 NVS
            if (battery->is_charge_in || (cap_calculate + 30 >= battery->nvs_cap && cap_calculate <= battery->nvs_cap + 15)) {
                cap_calculate = battery->nvs_cap;
            }
        }
        else{                    
            save_to_nvs = true;
        }
    
        battery->capacity = cap_calculate;
        // battery->batcap_is_init = true;        
        send_cap_event = true;
        ESP_LOGW(TAG, "battery voltage: %d, calculative cap: %d  display cap: %d", battery->voltage , cap_calculate,battery->capacity);
    }

    if (battery->bat_had_low  && battery->capacity != 0) {
        battery->capacity = 0;
        save_to_nvs = true;
        send_cap_event = true;
    }

    if(save_to_nvs){
        save_capacity_to_nvs(battery->capacity);
    }
    if(send_cap_event){
        send_bat_cap_event(self, battery->capacity);
    }

    return;

    
}

static portMUX_TYPE s_intr_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_intr_pending = false;
static volatile bool s_intr_cmd_queued = false;

static void IRAM_ATTR battery_gpio_intr_handler(void *param)
{
    esp_periph_handle_t periph = (esp_periph_handle_t)param;
    bool send_cmd = false;

    portENTER_CRITICAL_ISR(&s_intr_lock);
    s_intr_pending = true;
    if (!s_intr_cmd_queued) {
        s_intr_cmd_queued = true;
        send_cmd = true;
    }
    portEXIT_CRITICAL_ISR(&s_intr_lock);

    if (send_cmd && esp_periph_send_cmd_from_isr(periph, BM_EVENT_CHRG_STATUS_CHANGE, NULL, 0) != ESP_OK) {
        portENTER_CRITICAL_ISR(&s_intr_lock);
        s_intr_cmd_queued = false;
        portEXIT_CRITICAL_ISR(&s_intr_lock);
    }
}


// bool bat_is_low_voltage(void)
// {
//     /* if go here, sys state is not starting */
//     return bat_had_low;
// }

static esp_err_t _battery_run(esp_periph_handle_t self, audio_event_iface_msg_t *msg)
{
    //check charge in status
    periph_battery_t *battery = esp_periph_get_data(self);
    assert(battery != NULL);

    if (msg->cmd == BM_EVENT_CHRG_STATUS_CHANGE && !battery->batcap_is_init) {
        portENTER_CRITICAL(&s_intr_lock);
        s_intr_pending = false;
        s_intr_cmd_queued = false;
        portEXIT_CRITICAL(&s_intr_lock);
    } else if (msg->cmd == BM_EVENT_CHRG_STATUS_CHANGE) {
        bool requeue;

        portENTER_CRITICAL(&s_intr_lock);
        s_intr_pending = false;
        portEXIT_CRITICAL(&s_intr_lock);

        chg_state_change_notify();

        portENTER_CRITICAL(&s_intr_lock);
        requeue = s_intr_pending;
        if (!requeue) {
            s_intr_cmd_queued = false;
        }
        portEXIT_CRITICAL(&s_intr_lock);

        if (requeue && esp_periph_send_cmd(self, BM_EVENT_CHRG_STATUS_CHANGE, NULL, 0) != ESP_OK) {
            portENTER_CRITICAL(&s_intr_lock);
            s_intr_cmd_queued = false;
            portEXIT_CRITICAL(&s_intr_lock);
        }
        return ESP_OK;
    }
    periph_battery_snap_t snap = {0};
    battery->get_snap(&snap);

    bool new_ischarging =  snap.ischarging; 
    
    ESP_LOGD(TAG,"charge_status: %d", new_ischarging);
    if(battery->is_charge_in != new_ischarging){
        chg_state_change_notify();
        battery->is_charge_in = new_ischarging;
        battery->full_cnt = 0;
        battery->had_charge_active = false;
        battery->full_reported = false;
        if( battery->is_charge_in ){
            esp_periph_send_event(self, BM_EVENT_CHRG_IN, NULL, 0);
        }
        else{
            esp_periph_send_event(self, BM_EVENT_CHRG_OUT, NULL, 0);
            vTaskDelay(1/portTICK_PERIOD_MS);
            if(battery->batcap_is_init) send_bat_cap_event(self, battery->capacity);
            // send_event = true;
        }
    }
    
    else if (battery->is_charge_in) {
        if (snap.charge_active) {
            battery->had_charge_active = true;
            battery->full_reported = false;
            battery->full_cnt = 0;
        } else if (battery->had_charge_active && snap.is_full) {
            if (battery->full_cnt < CONFIG_CHARGE_FULL_DEBOUNCE_CNT) {
                battery->full_cnt++;
            }
            if (battery->full_cnt >= CONFIG_CHARGE_FULL_DEBOUNCE_CNT && !battery->full_reported) {
                esp_periph_send_event(self, BM_EVENT_CHRG_CMPL, NULL, 0);
                battery->full_reported = true;
                if (battery->capacity != 100) {
                    send_bat_cap_event(self, 100);
                    battery->capacity = 100;
                    save_capacity_to_nvs(100); /* 强制触发 NVS 保存 100% */
                }
            }
        } else {
            battery->full_cnt = 0;
        }
    }
    


    //get battery voltage
    battery->voltage = snap.vbat;

    //calculate battery capacity
    battery_handle_low(self);
    battery_handle_cap(self);
    if(battery->batcap_is_init == false){
        esp_periph_stop_timer(self);
        esp_periph_start_timer(self, 1000/portTICK_RATE_MS, bm_timer_handler);
        battery->batcap_is_init = true;
    }

    return ESP_OK;
}



static esp_err_t _battery_init(esp_periph_handle_t self)
{
    ESP_LOGW(TAG,"on init");
    periph_battery_t *battery = esp_periph_get_data(self);
    //set timer
    esp_periph_start_timer(self, 200/portTICK_RATE_MS, bm_timer_handler);
    // Set PINs
    // charge in GPIO
    gpio_set_direction(battery->intr_gpio, GPIO_MODE_INPUT);
    gpio_set_intr_type(battery->intr_gpio, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(battery->intr_gpio, battery_gpio_intr_handler, self);
    gpio_intr_enable(battery->intr_gpio);



    
    return ESP_OK;
}


esp_periph_handle_t periph_battery_init(periph_battery_cfg_t *cfg)
{
    esp_periph_handle_t periph = esp_periph_create(PERIPH_ID_BATTERY, "battery");
    
    periph_battery_t *battery = audio_calloc(1, sizeof(periph_battery_t));
    AUDIO_MEM_CHECK(TAG, battery, {
        audio_free(periph);
        return NULL;
    });
    memset(battery,0,sizeof(periph_battery_t));
    battery->intr_gpio = cfg->intr_gpio;
    battery->get_snap = cfg->get_snap;
    esp_periph_set_data(periph, battery);
    esp_periph_set_function(periph, _battery_init, _battery_run, _battery_destroy);
        // 从NVS中读取电池电量
    battery->nvs_cap = 0xFFFF; // 初始化为无效值
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    } else {
        uint16_t _nvs_cap = 0;
        err = nvs_get_u16(nvs_handle, "bat_cap", &_nvs_cap);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Error (%s) reading! Ignore when first run\n", esp_err_to_name(err));
        }
        else{
            ESP_LOGW(TAG, "Read NVS battery capacity: %d", _nvs_cap);
            if(_nvs_cap <= 100 ){
                battery->nvs_cap = _nvs_cap;
            }
        }
        nvs_close(nvs_handle);
    }
    // 在初始化时创建任务
    nvs_write_queue = xQueueCreate(5, sizeof(uint16_t));
    xTaskCreate(nvs_write_task, "nvs_write", 2048, NULL, 15, NULL);
    return periph;
}
