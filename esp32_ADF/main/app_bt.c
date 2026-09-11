#include "esp_log.h"
#include "app_bt.h"
#include "a2dp_stream.h"
#include "app_ble.h"
#include <string.h>
#include "app_player.h"
#include "lib/fw_timer.h"

static const char *TAG = "APP_BT";

static bool bt_connectable_flag = false;

static esp_bd_addr_t g_bda;
static esp_bd_addr_t last_connected_bdaddr;

#define TIMER_ID_DISCOVERABLE 1

esp_periph_handle_t bt_periph = NULL;







static void bt_timer_callback(U16 timerId, void *msg)
{
    switch(timerId)
    {
        case TIMER_ID_DISCOVERABLE:
            ESP_LOGI(TAG, "Discoverable timeout, switching to non-discoverable mode");
            
            if(app_player_is_a2dp_connected()){
                ESP_LOGI(TAG, "A2DP is connected, keeping discoverable mode");
                return;
            }
        #if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0))
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            bt_connectable_flag = true;
        #else
            esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE);
        #endif
            displayer_bt_blink_slow();
            // 一次性定时器超时后释放
            FW_ReleaseTimer(bt_timer_callback, TIMER_ID_DISCOVERABLE);
            break;
        default:
            break;
    }
}

esp_periph_handle_t app_bt_init(esp_periph_set_handle_t set)
{
#if CONFIG_BT_ENABLED
    ESP_LOGI(TAG, "Init Bluetooth module");
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    bt_cfg.controller_task_stack_size += 2048;
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    app_ble_init();

    // 获取并打印已配对设备列表
    int dev_num = esp_bt_gap_get_bond_device_num();
    ESP_LOGI(TAG, "-------Number of paired devices: %d---------", dev_num);
    if (dev_num > 0) {
        esp_bd_addr_t *dev_list = (esp_bd_addr_t *)malloc(sizeof(esp_bd_addr_t) * dev_num);
        if (dev_list != NULL) {
            if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK) {
                for (int i = 0; i < dev_num; i++) {
                    ESP_LOGI(TAG, "Paired device %d address: %02x:%02x:%02x:%02x:%02x:%02x",
                            i,
                            dev_list[i][0], dev_list[i][1], dev_list[i][2],
                            dev_list[i][3], dev_list[i][4], dev_list[i][5]);
                }
                memcpy(last_connected_bdaddr, dev_list[0], sizeof(esp_bd_addr_t));
            }
            free(dev_list);
        }
    }
    ESP_LOGI(TAG, "-------End of paired devices---------");

    
    esp_err_t set_dev_name_ret;
    // set_dev_name_ret = esp_bt_dev_set_device_name(DEFAULT_DEVICE_NAME);

    uint8_t *mac = esp_bt_dev_get_address();
    char device_name[32];
    snprintf(device_name, sizeof(device_name), "%s-%02X%02X", DEFAULT_DEVICE_NAME, mac[4], mac[5]);
    set_dev_name_ret = esp_bt_dev_set_device_name(device_name);
    
    if (set_dev_name_ret) {
        ESP_LOGE(TAG, "set device name failed, error code = %x", set_dev_name_ret);
    }
    bt_periph = bt_create_periph();
    

    esp_periph_start(set, bt_periph);
    ESP_LOGI(TAG, "Start Bluetooth peripherals");
    // board_display_set_bt_state(MX_BT_START);
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0))
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    // esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    // bt_connectable_flag = true;
#else
    esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_NONE);
#endif
    return bt_periph;
#else
    ESP_LOGE(TAG, "Please enable bt first");
    return NULL;
#endif
}

esp_err_t app_bt_non_discoverable(void){
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    bt_connectable_flag = true;
    displayer_bt_blink_slow();

    FW_ReleaseTimer(bt_timer_callback, TIMER_ID_DISCOVERABLE);
    return ESP_OK;
}

esp_err_t app_bt_dicoverable(void)
{
    esp_err_t ret = ESP_OK;
    if(bt_periph == NULL){
        ESP_LOGE(TAG, "bt_periph is NULL");
        return ESP_FAIL;
    }
#if CONFIG_BT_ENABLED
    ESP_LOGI(TAG, "bt dicoverable");

    // 使用 FW_SetTimer，如果同一个 (handler, timerId) 已存在，会自动重置
    
    #if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0))
    ret |= esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    #else
    ret |= esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_CONNECTABLE_DISCOVERABLE);
    #endif
    
    // 设置60秒(60000ms)的一次性定时器
    if (FW_SetTimer(bt_timer_callback, TIMER_ID_DISCOVERABLE, NULL, 60000) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start discoverable timer");
        ret |= ESP_FAIL;
    }
    bt_connectable_flag = true;
    displayer_bt_blink_fast();
    // board_display_set_bt_state(MX_BT_PAIRNG);
#endif
    return ret;
}

// esp_err_t app_bt_start_or_stop(){
//     esp_err_t ret = ESP_OK;
//     if(bt_periph == NULL){
//         ESP_LOGE(TAG, "bt_periph is NULL");
//         return ESP_FAIL;
//     }
//     if (bt_connectable_flag == false) {
//         ESP_LOGE(TAG, "BT ON");
//         // ret |= esp_a2d_sink_init();
//         ret |= esp_avrc_ct_init();//
//         ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
//         bt_connectable_flag = true;
//         board_display_set_bt_state(MX_BT_START);
//     }else{
//         ESP_LOGE(TAG, "BT OFF");
//         ret = esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
//         ret |= esp_a2d_sink_disconnect(g_bda);
//         // ret |= esp_a2d_sink_deinit();
//         ret |= esp_avrc_ct_deinit();
//         bt_connectable_flag = false;
//         board_display_set_bt_state(MX_BT_BREAK);   
//     }
//     return ret;
// }

esp_err_t app_bt_start(){
    esp_err_t ret = ESP_OK;
    if(bt_periph == NULL){
        ESP_LOGE(TAG, "bt_periph is NULL");
        return ESP_FAIL;
    }
    if (bt_connectable_flag == false) {
        ESP_LOGW(TAG, "BT ON");
        // ret |= esp_a2d_sink_init();
        // ret |= esp_avrc_ct_init();//
        ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
        app_player_a2dp_auto_reconnect_start();
        bt_connectable_flag = true;
        displayer_bt_blink_slow();
    }
    return ret;
}

esp_err_t app_bt_stop(void)
{
    esp_err_t ret = ESP_OK;
#if CONFIG_BT_ENABLED
    ESP_LOGI(TAG, "bt stop");
    if (bt_connectable_flag == true) {
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0))
        ret |= esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
#else
        ret |= esp_bt_gap_set_scan_mode(ESP_BT_SCAN_MODE_NONE);
#endif
        // if g_bda is not zero, disconnect
        if (memcmp(g_bda, "\0\0\0\0\0\0", ESP_BD_ADDR_LEN) != 0) {
            ret |= esp_a2d_sink_disconnect(g_bda);
        } else {
        }
        ret |= esp_a2d_sink_deinit();
        ret |= esp_avrc_ct_deinit();//
        bt_connectable_flag = false;
        displayer_bt_clear();
        app_player_a2dp_auto_reconnect_stop();
    }
#endif
    return ret;
}

void app_bt_deinit(void)
{
#if CONFIG_BT_ENABLED
    FW_ReleaseTimer(bt_timer_callback, TIMER_ID_DISCOVERABLE);
    app_player_a2dp_deinit();
    ESP_LOGI(TAG, "Deinit Bluetooth module");
    ESP_ERROR_CHECK(esp_bluedroid_disable());
    ESP_ERROR_CHECK(esp_bluedroid_deinit());
    ESP_ERROR_CHECK(esp_bt_controller_disable());
    ESP_ERROR_CHECK(esp_bt_controller_deinit());
    esp_periph_destroy(bt_periph);
    bt_periph = NULL;
    
    displayer_bt_clear();
#endif
}
esp_periph_handle_t app_bt_get_periph()
{
    return bt_periph;
}
void app_bt_set_addr(esp_bd_addr_t *addr)
{
    if (addr) {
        memcpy(&g_bda, addr, sizeof(esp_bd_addr_t));
    }
}

bool app_bt_get_connectable(){
    return bt_connectable_flag;
}

esp_bd_addr_t* app_bt_get_last_connected_bdaddr(){
    return &last_connected_bdaddr;
}

static void bt_recovery_call_back(void* arg){
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    app_player_a2dp_disconnect();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    app_bt_dicoverable();
    vTaskDelete(NULL);
}

esp_err_t app_bt_recovery(){
    ESP_LOGI(TAG, "bt recovery");
    //删除已配对的所有蓝牙列表
    displayer_bt_blink_fast();  // 应该是闪烁三次
    int dev_num = esp_bt_gap_get_bond_device_num();
    if (dev_num > 0) {
        esp_bd_addr_t *dev_list = (esp_bd_addr_t *)malloc(sizeof(esp_bd_addr_t) * dev_num);
        if (dev_list != NULL) {
            if (esp_bt_gap_get_bond_device_list(&dev_num, dev_list) == ESP_OK) {
                for (int i = 0; i < dev_num; i++) {
                    esp_bt_gap_remove_bond_device(dev_list[i]);
                    ESP_LOGI(TAG, "Removed paired device %d address: %02x:%02x:%02x:%02x:%02x:%02x",
                            i,
                            dev_list[i][0], dev_list[i][1], dev_list[i][2],
                            dev_list[i][3], dev_list[i][4], dev_list[i][5]);
                }
            }
            free(dev_list);
        }
    }
    //创建一个任务，等待1秒后断开蓝牙连接
    xTaskCreate(bt_recovery_call_back, "bt_recovery_call_back", 2048, NULL, 5, NULL);
    return ESP_OK;

}