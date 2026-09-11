#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "app_ble.h"
#include "errno.h"
#include "esp_log.h"
#include "esp_system.h"
//for ota 
#include "app_gatt_ota.h"
#include "app_gatt_device.h"
#include "app_gatt_uart.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "app_bt.h"
#include "esp_bt_device.h"
// #define CONFIG_SET_RAW_ADV_DATA 


static const char *TAG = "APP_GATT";



#define ADV_CONFIG_FLAG             (1 << 0)
#define SCAN_RSP_CONFIG_FLAG        (1 << 1)

static uint8_t adv_config_done       = 0;
static int s_ble_connection_count = 0;

//#define CONFIG_SET_RAW_ADV_DATA
#ifdef CONFIG_SET_RAW_ADV_DATA
static uint8_t raw_adv_data[] = {
        /* flags */
        0x02, 0x01, 0x06,
        /* tx power*/
        0x02, 0x0a, 0xeb,
        /* service uuid */
        0x03, 0x03, 0xFF, 0x00,
        /* device name */
        0x0f, 0x09, 'E', 'S', 'P', '_', 'G', 'A', 'T', 'T', 'S', '_', 'D','E', 'M', 'O'
};
static uint8_t raw_scan_rsp_data[] = {
        /* flags */
        0x02, 0x01, 0x06,
        /* tx power */
        0x02, 0x0a, 0xeb,
        /* service uuid */
        0x03, 0x03, 0xFF,0x00
};

#else


/* The length of adv data must be less than 31 bytes */
static esp_ble_adv_data_t adv_data = {
    .set_scan_rsp        = false,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006, //slave connection min interval, Time = min_interval * 1.25 msec
    .max_interval        = 0x0010, //slave connection max interval, Time = max_interval * 1.25 msec
    .appearance          = 0x00,
    .manufacturer_len    = 0,    //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL, //test_manufacturer,
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = 0,
    .p_service_uuid      = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

// scan response data
static esp_ble_adv_data_t scan_rsp_data = {
    .set_scan_rsp        = true,
    .include_name        = true,
    .include_txpower     = true,
    .min_interval        = 0x0006,
    .max_interval        = 0x0010,
    .appearance          = 0x00,
    .manufacturer_len    = 0, //TEST_MANUFACTURER_DATA_LEN,
    .p_manufacturer_data = NULL, //&test_manufacturer[0],
    .service_data_len    = 0,
    .p_service_data      = NULL,
    .service_uuid_len    = 0,
    .p_service_uuid      = NULL,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};
#endif /* CONFIG_SET_RAW_ADV_DATA */







static esp_ble_adv_params_t adv_params = {
    .adv_int_min         = 0x20,
    .adv_int_max         = 0x40,
    .adv_type            = ADV_TYPE_IND,
    .own_addr_type       = BLE_ADDR_TYPE_PUBLIC,
    .channel_map         = ADV_CHNL_ALL,
    .adv_filter_policy   = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

struct gatts_profile_inst {
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};



/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT */
static struct gatts_profile_inst gl_profile_tab[GATT_PROFILE_NUM] = {
    [GATT_PROFILE_ID_DEVICE] = {
        .gatts_cb = app_gatts_device_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },    
    [GATT_PROFILE_ID_OTA] = {
        .gatts_cb = app_gatt_ota_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,       /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
    },
    [GATT_PROFILE_ID_UART] = {
        .gatts_cb = app_gatts_uart_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    },
};





static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    // ESP_LOGI(TAG,"GATT EVENTS: %d",(int)event);
    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT) {
        if (param->reg.status == ESP_GATT_OK) {
            // heart_rate_profile_tab[PROFILE_APP_IDX].gatts_if = gatts_if;
            gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;

            if(param->reg.app_id == 0){
                esp_err_t set_dev_name_ret;
                char device_name[32];
                uint8_t* mac = esp_bt_dev_get_address();
                snprintf(device_name, sizeof(device_name), "%s-%02X%02X", DEFAULT_DEVICE_NAME, mac[4], mac[5]);
                set_dev_name_ret = esp_bt_dev_set_device_name(device_name);
                if (set_dev_name_ret){
                    ESP_LOGE(TAG, "set device name failed, error code = %x", set_dev_name_ret);
                }
                #ifdef CONFIG_SET_RAW_ADV_DATA
                esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(raw_adv_data, sizeof(raw_adv_data));
                if (raw_adv_ret){
                    ESP_LOGE(TAG, "config raw adv data failed, error code = %x ", raw_adv_ret);
                }
                adv_config_done |= ADV_CONFIG_FLAG;
                esp_err_t raw_scan_ret = esp_ble_gap_config_scan_rsp_data_raw(raw_scan_rsp_data, sizeof(raw_scan_rsp_data));
                if (raw_scan_ret){
                    ESP_LOGE(TAG, "config raw scan rsp data failed, error code = %x", raw_scan_ret);
                }
                adv_config_done |= SCAN_RSP_CONFIG_FLAG;
                #else
                //config adv data
                esp_err_t ret = esp_ble_gap_config_adv_data(&adv_data);
                if (ret){
                    ESP_LOGE(TAG, "config adv data failed, error code = %x", ret);
                }
                adv_config_done |= ADV_CONFIG_FLAG;
                //config scan response data
                ret = esp_ble_gap_config_adv_data(&scan_rsp_data);
                if (ret){
                    ESP_LOGE(TAG, "config scan response data failed, error code = %x", ret);
                }
                adv_config_done |= SCAN_RSP_CONFIG_FLAG;
                #endif
            }

        } else {
            ESP_LOGE(TAG, "reg app failed, app_id %04x, status %d",
                    param->reg.app_id,
                    param->reg.status);
            return;
        }
    }
    if(event == ESP_GATTS_DISCONNECT_EVT){        
        ESP_LOGI(TAG, "ESP_GATTS_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);
        if (s_ble_connection_count > 0) {
            s_ble_connection_count--;
        }
        esp_ble_gap_start_advertising(&adv_params);        
  
    }

    if(event == ESP_GATTS_CONNECT_EVT){
        ESP_LOGI(TAG, "ESP_GATTS_CONNECT_EVT, conn_id = %d", param->connect.conn_id);
        s_ble_connection_count++;
        esp_log_buffer_hex(TAG, param->connect.remote_bda, 6);
        esp_ble_conn_update_params_t conn_params = {0};
        memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
        /* For the iOS system, please refer to Apple official documents about the BLE connection parameters restrictions. */
        conn_params.latency = 0;
        conn_params.max_int = 0x20;    // max_int = 0x20*1.25ms = 40ms
        conn_params.min_int = 0x10;    // min_int = 0x10*1.25ms = 20ms
        conn_params.timeout = 400;    // timeout = 400*10ms = 4000ms
        //start sent the update connection parameters to the peer device.
        esp_ble_gap_update_conn_params(&conn_params);
    }
    do {
        int idx;
        for (idx = 0; idx < GATT_PROFILE_NUM; idx++) {
            /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every profile cb function */
            if (gatts_if == ESP_GATT_IF_NONE || gatts_if == gl_profile_tab[idx].gatts_if) {
                if (gl_profile_tab[idx].gatts_cb) {
                    gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}


static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    #ifdef CONFIG_SET_RAW_ADV_DATA
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
            adv_config_done &= (~ADV_CONFIG_FLAG);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;
    #else
        case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~ADV_CONFIG_FLAG);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;
        case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
            adv_config_done &= (~SCAN_RSP_CONFIG_FLAG);
            if (adv_config_done == 0){
                esp_ble_gap_start_advertising(&adv_params);
            }
            break;
    #endif
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            /* advertising start complete event to indicate advertising start successfully or failed */
            if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "advertising start failed");
            }else{
                ESP_LOGI(TAG, "advertising start successfully");
            }
            break;
        case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
            if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
                ESP_LOGE(TAG, "Advertising stop failed");
            }
            else {
                ESP_LOGI(TAG, "Stop adv successfully\n");
            }
            break;
        case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
            ESP_LOGI(TAG, "update connection params status = %d, min_int = %d, max_int = %d,conn_int = %d,latency = %d, timeout = %d",
                  param->update_conn_params.status,
                  param->update_conn_params.min_int,
                  param->update_conn_params.max_int,
                  param->update_conn_params.conn_int,
                  param->update_conn_params.latency,
                  param->update_conn_params.timeout);
            break;
        default:
            break;
    }
}



esp_err_t app_ble_init(void){
    esp_err_t ret;
    // ESP_LOGE(TAG,"%s",__TIME__);
    ret = esp_ble_gatts_register_callback(gatts_event_handler);
    if (ret){
        ESP_LOGE(TAG, "gatts register error, error code = %x", ret);
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret){
        ESP_LOGE(TAG, "gap register error, error code = %x", ret);
        return ret;
    }
    ret = esp_ble_gatts_app_register(GATT_PROFILE_ID_DEVICE);
    ret = esp_ble_gatts_app_register(GATT_PROFILE_ID_OTA);
    ret = esp_ble_gatts_app_register(GATT_PROFILE_ID_UART);
    if (ret){
        ESP_LOGE(TAG, "gatts app register error, error code = %x", ret);
        return ret;
    }

    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret){
        ESP_LOGE(TAG, "set local  MTU failed, error code = %x", local_mtu_ret);
    }


    return ret;
}
bool app_ble_is_connected(void)
{
    return s_ble_connection_count > 0;
}
