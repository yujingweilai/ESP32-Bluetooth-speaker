#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "app_gatt_ota.h"
#include "esp_ota_ops.h"
#include "esp_log.h"
#include "esp_system.h"
#include "app_ble.h"
#include "esp_app_format.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "app_bt.h"


#define UUID_NRF_OTA
/* The max length of characteristic value. When the GATT client performs a write or prepare write operation,
*  the data length must be less than GATTS_CHAR_VAL_LEN_MAX.
*/





static char * TAG = "APP_GATT_OTA";

enum
{
    OTA_SERVICE_IDX_SVC,
    OTA_SERVICE_IDX_CHAR_OTA_CTRL,
    OTA_SERVICE_IDX_CHAR_VAL_OTA_CTRL,

    OTA_SERVICE_IDX_CHAR_OTA_DATA,
    OTA_SERVICE_IDX_CHAR_VAL_OTA_DATA,

    OTA_SERVICE_IDX_NB,
};









/* update handle : set by esp_ota_begin(), must be freed via esp_ota_end() */

static prepare_type_env_t prepare_write_env;

static uint16_t service_handle_table[OTA_SERVICE_IDX_NB];
static uint16_t ota_last_ctrl_handle = 0;
static uint16_t ota_last_data_handle = 0;
static uint8_t service_uuid[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    //0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    #ifdef UUID_NRF_OTA
    0xf0, 0x19, 0x21, 0xb4, 0x47, 0x8f, 0xa4, 0xbf, 0xa1, 0x4f, 0x63, 0xfd, 0xee, 0xd6, 0x14, 0x1d,
    #else
    #endif
};

/*
Type: com.silabs.characteristic.ota_control
UUID: F7BF3564-FB6D-4E53-88A4-5E37E0326063
Silicon Labs OTA Control.
Property requirements: 
	Notify - Excluded
	Read - Excluded
	Write Without Response - Excluded
	Write - Mandatory
	Reliable write - Excluded
	Indicate - Excluded
*/
static uint8_t char_ota_control_uuid[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    //0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    #ifdef UUID_NRF_OTA
    0x63, 0x60, 0x32, 0xe0, 0x37, 0x5e, 0xa4, 0x88, 0x53, 0x4e, 0x6d, 0xfb, 0x64, 0x35, 0xbf, 0xf7,
    #else
    #endif
};


/*
Type: com.silabs.characteristic.ota_data
UUID: 984227F3-34FC-4045-A5D0-2C581F81A153
Silicon Labs OTA Data.
Property requirements: 
	Notify - Excluded
	Read - Excluded
	Write Without Response - Mandatory
	Write - Mandatory
	Reliable write - Excluded
	Indicate - Excluded
*/
static uint8_t char_ota_data_uuid[16] = {
    /* LSB <--------------------------------------------------------------------------------> MSB */
    //first uuid, 16bit, [12],[13] is the value
    //0xfb, 0x34, 0x9b, 0x5f, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00,
    #ifdef UUID_NRF_OTA
    0x53, 0xa1, 0x81, 0x1f, 0x58, 0x2c, 0xd0, 0xa5, 0x45, 0x40, 0xfc, 0x34, 0xf3, 0x27, 0x42, 0x98,
    #else
    #endif
};
static const uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint8_t char_value[4]                 = {0x11, 0x22, 0x33, 0x44};

static const uint8_t char_prop_write_writenorsp    =  ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;




/* Full Database Description - Used to add attributes into the database */
static const esp_gatts_attr_db_t gatt_db[OTA_SERVICE_IDX_NB] =
{
    // Service Declaration
    [OTA_SERVICE_IDX_SVC]        =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
      sizeof(service_uuid), sizeof(service_uuid), (uint8_t *)&service_uuid}},

    /* Characteristic Declaration */
    [OTA_SERVICE_IDX_CHAR_OTA_CTRL]     =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write_writenorsp}},

    /* Characteristic Value */
    [OTA_SERVICE_IDX_CHAR_VAL_OTA_CTRL] =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)&char_ota_control_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},


    /* Characteristic Declaration */
    [OTA_SERVICE_IDX_CHAR_OTA_DATA]      =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
      CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_write_writenorsp}},

    /* Characteristic Value */
    [OTA_SERVICE_IDX_CHAR_VAL_OTA_DATA]  =
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)&char_ota_data_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
      GATTS_CHAR_VAL_LEN_MAX, sizeof(char_value), (uint8_t *)char_value}},
};

static void example_prepare_write_event_env(esp_gatt_if_t gatts_if, prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param)
{
    ESP_LOGI(TAG, "prepare write, handle = %d, value len = %d", param->write.handle, param->write.len);
    esp_gatt_status_t status = ESP_GATT_OK;
    if (prepare_write_env->prepare_buf == NULL) {
        prepare_write_env->prepare_buf = (uint8_t *)malloc(PREPARE_BUF_MAX_SIZE * sizeof(uint8_t));
        prepare_write_env->prepare_len = 0;
        if (prepare_write_env->prepare_buf == NULL) {
            ESP_LOGE(TAG, "%s, Gatt_server prep no mem", __func__);
            status = ESP_GATT_NO_RESOURCES;
        }
    } else {
        if(param->write.offset > PREPARE_BUF_MAX_SIZE) {
            status = ESP_GATT_INVALID_OFFSET;
        } else if ((param->write.offset + param->write.len) > PREPARE_BUF_MAX_SIZE) {
            status = ESP_GATT_INVALID_ATTR_LEN;
        }
    }
    /*send response when param->write.need_rsp is true */
    if (param->write.need_rsp){
        esp_gatt_rsp_t *gatt_rsp = (esp_gatt_rsp_t *)malloc(sizeof(esp_gatt_rsp_t));
        if (gatt_rsp != NULL){
            gatt_rsp->attr_value.len = param->write.len;
            gatt_rsp->attr_value.handle = param->write.handle;
            gatt_rsp->attr_value.offset = param->write.offset;
            gatt_rsp->attr_value.auth_req = ESP_GATT_AUTH_REQ_NONE;
            memcpy(gatt_rsp->attr_value.value, param->write.value, param->write.len);
            esp_err_t response_err = esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, status, gatt_rsp);
            if (response_err != ESP_OK){
               ESP_LOGE(TAG, "Send response error");
            }
            free(gatt_rsp);
        }else{
            ESP_LOGE(TAG, "%s, malloc failed", __func__);
        }
    }
    if (status != ESP_GATT_OK){
        return;
    }
    memcpy(prepare_write_env->prepare_buf + param->write.offset,
           param->write.value,
           param->write.len);
    prepare_write_env->prepare_len += param->write.len;

}

void example_exec_write_event_env(prepare_type_env_t *prepare_write_env, esp_ble_gatts_cb_param_t *param){
    if (param->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && prepare_write_env->prepare_buf){
        esp_log_buffer_hex(TAG, prepare_write_env->prepare_buf, prepare_write_env->prepare_len);
    }else{
        ESP_LOGI(TAG,"ESP_GATT_PREP_WRITE_CANCEL");
    }
    if (prepare_write_env->prepare_buf) {
        free(prepare_write_env->prepare_buf);
        prepare_write_env->prepare_buf = NULL;
    }
    prepare_write_env->prepare_len = 0;
}

typedef struct {
    uint8_t header_data[PREPARE_BUF_MAX_SIZE+1];
    bool header_was_checked;
    uint16_t header_len;
    uint32_t total_len;
    esp_ota_handle_t update_handle;
    const esp_partition_t *update_partition;
} ota_image_handle_t;
ota_image_handle_t *ota_image_handle;


void ota_clean_image_header_data()
{
    if(ota_image_handle){
        ESP_LOGW(TAG, "OTA handle freed");
        free(ota_image_handle);
        ota_image_handle = NULL;
    }
    
}
extern void app_poweroff_loader();
void ota_restart_task(void *param)
{
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        uint8_t ota_done = 1;
        err = nvs_set_u8(my_handle, "ota_done", ota_done);
        if (err == ESP_OK) {
            uint8_t bt_conn = app_bt_get_connectable() ? 1 : 0;
            err = nvs_set_u8(my_handle, "ota_bt_conn", bt_conn);
            if (err == ESP_OK) {
                err = nvs_commit(my_handle);
                if (err == ESP_OK) {
                    ESP_LOGI(TAG, "OTA done flag and BT conn flag set in NVS");
                } else {
                    ESP_LOGE(TAG, "Failed to commit NVS");
                }
            } else {
                ESP_LOGE(TAG, "Failed to set BT conn NVS value");
            }
        } else {
            ESP_LOGE(TAG, "Failed to set NVS value");
        }
        nvs_close(my_handle);
    } else {
        ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "Prepare to restart system!");
    app_poweroff_loader();
    vTaskDelay(200 / portTICK_PERIOD_MS);

    // board_shut_down();
    esp_restart();

    vTaskDelete(NULL);
}



void app_gatt_ota_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    esp_err_t err;
    // ESP_LOGI(TAG, "gatts profile event  %d", event);
    switch (event) {
        case ESP_GATTS_REG_EVT:{

            esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, OTA_SERVICE_IDX_NB, 0x00);
            if (create_attr_ret){
                ESP_LOGE(TAG, "create attr table failed, error code = %x", create_attr_ret);
            }
        }
       	    break;
        case ESP_GATTS_READ_EVT:
            ESP_LOGI(TAG, "ESP_GATTS_READ_EVT");
       	    break;
        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep){
                // the data length of gattc write  must be less than GATTS_CHAR_VAL_LEN_MAX.
                ESP_LOGD(TAG, "GATT_WRITE_EVT, handle = %d, value len = %d, value:", param->write.handle, param->write.len);
                //esp_log_buffer_hex(TAG, param->write.value, param->write.len);
                
                //OTA control
				if (service_handle_table[OTA_SERVICE_IDX_CHAR_VAL_OTA_CTRL] == param->write.handle && param->write.len == 1){
                    uint8_t value = param->write.value[0];
					ESP_LOGI(TAG, "ota-control = %d",value);
                    if(0x00 == value){
						ESP_LOGI(TAG, "======begin ota======");
                        ota_last_ctrl_handle = param->write.handle;
                        ota_last_data_handle = 0;



                        if(ota_image_handle != NULL){
                            free(ota_image_handle);
                        }
                        ota_image_handle = (ota_image_handle_t *)malloc(sizeof(ota_image_handle_t));
                        if( ota_image_handle ){
                            ota_image_handle->header_was_checked = false;
                            ota_image_handle->header_len = 0; 
                            ota_image_handle->total_len = 0;     
                            ota_image_handle->update_handle=0;
                            ota_image_handle->update_partition = NULL;

                        }else{
                            ESP_LOGE(TAG, "%s, malloc failed", __func__);
                            break;
                        }
						ota_image_handle->update_partition = esp_ota_get_next_update_partition(NULL);
 					    //  ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%x",
 					    //          update_partition->subtype, (unsigned int)update_partition->address);
                        if (ota_image_handle->update_partition == NULL) {
                            ESP_LOGE(TAG, "get next update partition failed");
                            ota_clean_image_header_data();
                            break;
                        }

  
					}
					else if(0x03 == value){
                        ota_last_ctrl_handle = param->write.handle;
                        ESP_LOGI(TAG, "OTA end requested, last ctrl handle=%u last data handle=%u",
                                 (unsigned)ota_last_ctrl_handle, (unsigned)ota_last_data_handle);
                        if (ota_image_handle == NULL) {
                            ESP_LOGE(TAG, "end ota but handle is NULL");
                            break;
                        }
                        if (ota_image_handle->update_partition == NULL) {
                            ESP_LOGE(TAG, "end ota but update partition is NULL");
                            ota_clean_image_header_data();
                            break;
                        }
                        if (ota_image_handle->update_handle == 0) {
                            ESP_LOGE(TAG, "end ota but update not started");
                            ota_clean_image_header_data();
                            break;
                        }
						ESP_LOGI(TAG, "======end ota======");
                        ESP_LOGI(TAG,"Total Write binary data length : %d",(int)ota_image_handle->total_len);
						err = esp_ota_end(ota_image_handle->update_handle);
					    if (err != ESP_OK) {
					        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
					            ESP_LOGE(TAG, "Image validation failed, image is corrupted");
					        }
					        ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
					    }

					    err = esp_ota_set_boot_partition(ota_image_handle->update_partition);
					    if (err != ESP_OK) {
					        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
	
					    }
                        // ota_clean_image_header_data();
					    // ESP_LOGI(TAG, "Prepare to restart system!");
					    // esp_restart();
                        xTaskCreate(ota_restart_task, "ota_restart", 4096, NULL, 5, NULL);
					    return ;
					}					
                }
                // OTA date
				if (service_handle_table[OTA_SERVICE_IDX_CHAR_VAL_OTA_DATA] == param->write.handle){
                    ota_last_data_handle = param->write.handle;
                    if( ota_image_handle ){
                        if(!ota_image_handle->header_was_checked){
                            memcpy(ota_image_handle->header_data+ota_image_handle->header_len, param->write.value, param->write.len);
                            ota_image_handle->header_len += param->write.len;
                            if( ota_image_handle->header_len> sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t) ){
                                esp_app_desc_t new_app_info;
                                memcpy(&new_app_info, &ota_image_handle->header_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));

                                esp_app_desc_t running_app_info;
                                const esp_partition_t *running = esp_ota_get_running_partition();
                                if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK) {
                                    ESP_LOGI(TAG, "Running firmware version: %s", running_app_info.version);
                                }

                                const esp_partition_t* last_invalid_app = esp_ota_get_last_invalid_partition();
                                esp_app_desc_t invalid_app_info;
                                if (esp_ota_get_partition_description(last_invalid_app, &invalid_app_info) == ESP_OK) {
                                    ESP_LOGI(TAG, "Last invalid firmware version: %s", invalid_app_info.version);
                                }

                                // check current version with last invalid partition
                                if (last_invalid_app != NULL) {
                                    if (memcmp(invalid_app_info.version, new_app_info.version, sizeof(new_app_info.version)) == 0) {
                                        ESP_LOGW(TAG, "New version is the same as invalid version.");
                                        ESP_LOGW(TAG, "Previously, there was an attempt to launch the firmware with %s version, but it failed.", invalid_app_info.version);
                                        ESP_LOGW(TAG, "The firmware has been rolled back to the previous version.");
                                        ESP_LOGW(TAG, "OTA abort: new version matches last invalid");
                                        ota_clean_image_header_data();
                                        break;
                                    }
                                }
    #ifndef CONFIG_EXAMPLE_SKIP_VERSION_CHECK
                                if (memcmp(new_app_info.version, running_app_info.version, sizeof(new_app_info.version)) == 0) {
                                    ESP_LOGW(TAG, "Current running version is the same as a new. We will not continue the update.");
                                    ESP_LOGW(TAG, "OTA abort: new version matches running");
                                    ota_clean_image_header_data();
                                    break;                                    
                                }
    #endif
                                ota_image_handle->header_was_checked = true;

                                err = esp_ota_begin(ota_image_handle->update_partition, OTA_WITH_SEQUENTIAL_WRITES, &ota_image_handle->update_handle);
                                if (err != ESP_OK) {
                                    ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
                                    esp_ota_abort(ota_image_handle->update_handle);
                                    ESP_LOGE(TAG, "OTA abort: esp_ota_begin failed");
                                }

                                err = esp_ota_write( ota_image_handle->update_handle, ota_image_handle->header_data, ota_image_handle->header_len);
                                if (err != ESP_OK) {
                                    esp_ota_abort(ota_image_handle->update_handle);
                                    ota_clean_image_header_data();
                                    ESP_LOGI(TAG, "esp_ota_write error!");
                                    ESP_LOGE(TAG, "OTA abort: header write failed");
                                    break;
                                }
                                ota_image_handle->total_len += ota_image_handle->header_len;
                            }

                        }
                        else{                    
                            uint16_t length = param->write.len;//modify uint8_t to uint16_t when mtu larger than 255
                            // ESP_LOGI(TAG, "ota-data = %d",length);
                            ota_image_handle->total_len += length;
                            err = esp_ota_write( ota_image_handle->update_handle, (const void *)param->write.value, length);
                            if (err != ESP_OK) {
                                esp_ota_abort(ota_image_handle->update_handle);
                                ota_clean_image_header_data();
                                ESP_LOGI(TAG, "esp_ota_write error!");
                                ESP_LOGE(TAG, "OTA abort: data write failed");
                            }                     
                        }
                    } else {
                        ESP_LOGE(TAG, "OTA data received but handle is NULL (ctrl handle=%u)",
                                 (unsigned)ota_last_ctrl_handle);
                    }

                }

				/* send response when param->write.need_rsp is true*/
                if (param->write.need_rsp){
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
                }
            }else{
                /* handle prepare write */
                example_prepare_write_event_env(gatts_if, &prepare_write_env, param);
            }
      	    break;
        case ESP_GATTS_EXEC_WRITE_EVT:
            // the length of gattc prepare write data must be less than GATTS_CHAR_VAL_LEN_MAX.
            ESP_LOGI(TAG, "ESP_GATTS_EXEC_WRITE_EVT");
            example_exec_write_event_env(&prepare_write_env, param);
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
            esp_ble_conn_update_params_t conn_params = {0};
            break;
        case ESP_GATTS_DISCONNECT_EVT:
            if(ota_image_handle){
                esp_ota_abort(ota_image_handle->update_handle);
                ota_clean_image_header_data();
            }
            ESP_LOGI(TAG, "ESP_GATTS_DISCONNECT_EVT, reason = 0x%x", param->disconnect.reason);

            // esp_ble_gap_start_advertising(&adv_params);
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:{
            if (param->add_attr_tab.status != ESP_GATT_OK){
                ESP_LOGE(TAG, "create attribute table failed, error code=0x%x", param->add_attr_tab.status);
            }
            else if (param->add_attr_tab.num_handle != OTA_SERVICE_IDX_NB){
                ESP_LOGE(TAG, "create attribute table abnormally, num_handle (%d) \
                        doesn't equal to SERVICE_IDX_NB(%d)", param->add_attr_tab.num_handle, OTA_SERVICE_IDX_NB);
            }
            else {
                ESP_LOGI(TAG, "create attribute table successfully, the number handle = %d\n",param->add_attr_tab.num_handle);
                memcpy(service_handle_table, param->add_attr_tab.handles, sizeof(service_handle_table));
                esp_ble_gatts_start_service(service_handle_table[OTA_SERVICE_IDX_SVC]);
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
