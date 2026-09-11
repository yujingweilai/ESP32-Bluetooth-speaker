
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "app_gatt_device.h"
#include "app_ble.h"
#include "esp_log.h"

static char * TAG = "APP_GATT_DEVICE";

static int s_battery_level = 0;

#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))

static const uint8_t char_prop_read_notify   = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;

static enum
{
    // BATTERY_SERVICE_IDX_SVC,
    DEVICE_INFO_SERVICE_IDX_SVC,
    CHAR_BATTERY_LEVE,
    CHAR_BATTERY_LEVE_VALUE,
    CHAR_FIRMWAREVE_VERSION,
    CHAR_FIRMWAREVE_VERSION_VALUE,


    SERVICE_IDX_NB,
};

static uint16_t service_handle_table[SERVICE_IDX_NB];
static const uint16_t character_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
// static uint8_t battery_service_uuid[2] = {0x0F,0x18};
static uint8_t device_info_service_uuid[2] = {0x0a,0x18};
static uint8_t firmware_version_char_uuid[2] = {0x26,0x2A};
static uint8_t battery_level_char_uuid[2] = {0x19,0x2A};
static const uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static const uint8_t battery_level[] = "0";
static uint8_t firmware_version[] = "000.000.000";
/* Full Database Description - Used to add attributes into the database */
static const esp_gatts_attr_db_t gatt_db[SERVICE_IDX_NB] =
{
    // Service Declaration
    [DEVICE_INFO_SERVICE_IDX_SVC]        =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
      sizeof(device_info_service_uuid), sizeof(device_info_service_uuid), (uint8_t *)&device_info_service_uuid}},

    [CHAR_BATTERY_LEVE]     =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},

    /* Characteristic Value */
    [CHAR_BATTERY_LEVE_VALUE] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&battery_level_char_uuid, ESP_GATT_PERM_READ ,
      GATTS_CHAR_VAL_LEN_MAX, sizeof(battery_level), (uint8_t *)&battery_level}},
    
    [CHAR_FIRMWAREVE_VERSION]     =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},
    /* Characteristic Value */
    [CHAR_FIRMWAREVE_VERSION_VALUE] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&firmware_version_char_uuid, ESP_GATT_PERM_READ ,
      GATTS_CHAR_VAL_LEN_MAX, sizeof(firmware_version), (uint8_t *)&firmware_version}},
};





void app_gatts_device_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT:{            
            esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, SERVICE_IDX_NB, 0);
            if (create_attr_ret){
                ESP_LOGE(TAG, "create attr table failed, error code = %x", create_attr_ret);
            }
        }
       	    break;
        case ESP_GATTS_READ_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_READ_EVT");
       	    break;
        case ESP_GATTS_WRITE_EVT:           
            // the data length of gattc write  must be less than GATTS_DEMO_CHAR_VAL_LEN_MAX.
            ESP_LOGI(TAG, "GATT_WRITE_EVT, handle = %d, value len = %d, value :", param->write.handle, param->write.len);

      	    break;
        case ESP_GATTS_EXEC_WRITE_EVT:
            // the length of gattc prepare write data must be less than GATTS_DEMO_CHAR_VAL_LEN_MAX.
            ESP_LOGI(TAG, "ESP_GATTS_EXEC_WRITE_EVT");
            break;
        case ESP_GATTS_MTU_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_MTU_EVT, MTU %d", param->mtu.mtu);
            break;
        case ESP_GATTS_CONF_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_CONF_EVT, status = %d, attr_handle %d", param->conf.status, param->conf.handle);
            break;
        case ESP_GATTS_START_EVT:
            ESP_LOGI(TAG, "SERVICE_START_EVT, status %d, service_handle %d", param->start.status, param->start.service_handle);
            break;
        case ESP_GATTS_CONNECT_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_CONNECT_EVT, conn_id = %d", param->connect.conn_id);
            esp_log_buffer_hex(TAG, param->connect.remote_bda, 6);
            break;
        case ESP_GATTS_DISCONNECT_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:{
            if (param->add_attr_tab.status != ESP_GATT_OK){
                ESP_LOGE(TAG, "create attribute table failed, error code=0x%x", param->add_attr_tab.status);
            }
            else if (param->add_attr_tab.num_handle != SERVICE_IDX_NB){
                ESP_LOGE(TAG, "create attribute table abnormally, num_handle (%d) \
                        doesn't equal to SERVICE_IDX_NB(%d)", param->add_attr_tab.num_handle, SERVICE_IDX_NB);
            }
            else {
                ESP_LOGI(TAG, "create attribute table successfully, the number handle = %d\n",param->add_attr_tab.num_handle);
                memcpy(service_handle_table, param->add_attr_tab.handles, sizeof(service_handle_table));
                
                // Sync buffered battery level
                uint8_t level = (uint8_t)s_battery_level;
                esp_ble_gatts_set_attr_value(service_handle_table[CHAR_BATTERY_LEVE_VALUE], sizeof(level), &level);

                esp_ble_gatts_start_service(service_handle_table[DEVICE_INFO_SERVICE_IDX_SVC]);
            }
            break;
        }
        case ESP_GATTS_STOP_EVT:
        case ESP_GATTS_OPEN_EVT:
        case ESP_GATTS_CANCEL_OPEN_EVT:
        case ESP_GATTS_CLOSE_EVT:
        case ESP_GATTS_LISTEN_EVT:
        case ESP_GATTS_CONGEST_EVT:
        case ESP_GATTS_UNREG_EVT:
        case ESP_GATTS_DELETE_EVT:
        default:
            break;
    }
}

void app_gatt_device_set_battery_level(int level)
{
    s_battery_level = level;

    // Only update if handle table is initialized (non-zero handle)
    if (service_handle_table[CHAR_BATTERY_LEVE_VALUE] != 0) {
        uint8_t battery_level_val = level;
        esp_ble_gatts_set_attr_value(service_handle_table[CHAR_BATTERY_LEVE_VALUE], sizeof(battery_level_val), (uint8_t *)&battery_level_val);
    } else {
        ESP_LOGD(TAG, "GATT not ready, buffering battery level: %d", level);
    }
}

void app_gatt_device_set_firmware_version( char * version )
{
    memcpy(firmware_version, version, strlen(version));
    esp_ble_gatts_set_attr_value(service_handle_table[CHAR_FIRMWAREVE_VERSION_VALUE], strlen(version), (uint8_t *)version);
}
