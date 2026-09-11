#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "app_gatt_uart.h"
#include "app_ble.h"
#include "esp_log.h"
#include "app_player.h"

static const char *TAG = "APP_GATT_UART";

enum {
    UART_SERVICE_IDX_SVC,
    UART_SERVICE_IDX_RX_CHAR,
    UART_SERVICE_IDX_RX_VAL,
    UART_SERVICE_IDX_TX_CHAR,
    UART_SERVICE_IDX_TX_VAL,
    UART_SERVICE_IDX_TX_CFG,
    UART_SERVICE_IDX_NB,
};

static uint16_t service_handle_table[UART_SERVICE_IDX_NB];
static uint8_t uart_service_uuid[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x01, 0x11, 0x00, 0x00
};

static uint8_t uart_rx_char_uuid[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x02, 0x11, 0x00, 0x00
};

static uint8_t uart_tx_char_uuid[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00, 0x03, 0x11, 0x00, 0x00
};

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t char_prop_read_write = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t char_prop_read_notify = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint8_t uart_tx_ccc[2] = {0x00, 0x00};

static const esp_gatts_attr_db_t gatt_db[UART_SERVICE_IDX_NB] = {
    [UART_SERVICE_IDX_SVC] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
                                                   sizeof(uart_service_uuid), sizeof(uart_service_uuid), (uint8_t *)uart_service_uuid}},

    [UART_SERVICE_IDX_RX_CHAR] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
                                                       CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},

    [UART_SERVICE_IDX_RX_VAL] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)uart_rx_char_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                      GATTS_CHAR_VAL_LEN_MAX, 0, NULL}},

    [UART_SERVICE_IDX_TX_CHAR] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
                                                       CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},

    [UART_SERVICE_IDX_TX_VAL] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t *)uart_tx_char_uuid, ESP_GATT_PERM_READ,
                                                      GATTS_CHAR_VAL_LEN_MAX, 0, NULL}},

    [UART_SERVICE_IDX_TX_CFG] = {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                                                      sizeof(uint16_t), sizeof(uart_tx_ccc), (uint8_t *)uart_tx_ccc}},
};

static uint16_t conn_id = 0xffff;
static esp_gatt_if_t uart_gatts_if = 0xff;

void app_gatts_uart_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
        case ESP_GATTS_REG_EVT: {
            esp_err_t create_attr_ret = esp_ble_gatts_create_attr_tab(gatt_db, gatts_if, UART_SERVICE_IDX_NB, 0);
            if (create_attr_ret) {
                ESP_LOGE(TAG, "create attr table failed, error code = %x", create_attr_ret);
            }
        } break;

        case ESP_GATTS_WRITE_EVT:
            if (!param->write.is_prep) {
                if (param->write.handle == service_handle_table[UART_SERVICE_IDX_RX_VAL]) {
                    ESP_LOGI(TAG, "UART RX DATA: %.*s", param->write.len, param->write.value);
                    
                    // 验证EQ参数
                    if (param->write.len > 6 && strncmp((char*)param->write.value, "SETEQ ", 6) == 0) {
                        char *data = (char*)malloc(param->write.len + 1);
                        memcpy(data, param->write.value, param->write.len);
                        data[param->write.len] = '\0';
                        
                        char *token = strtok(data + 6, " "); // 跳过"SETEQ "前缀
                        int param_count = 0;
                        int eq_params[10] = {0};
                        
                        // 解析10个参数
                        while (token != NULL && param_count < 10) {
                            eq_params[param_count] = atoi(token); // 将字符串转换为整数
                            param_count++;
                            token = strtok(NULL, " ");
                        }
                        
                        if (param_count == 10) {
                            ESP_LOGI(TAG, "Received valid EQ parameters:");
                            for (int i = 0; i < 10; i++) {
                                ESP_LOGI(TAG, "Parameter %d: %d", i+1, eq_params[i]);
                            }
                            app_player_set_eq(eq_params);
                            // 添加发送EQ参数和频率的应答
                            char response[256];
                            snprintf(response, sizeof(response), 
                                "set success "
                                "31Hz:%d "
                                "62Hz:%d "
                                "125Hz:%d "
                                "250Hz:%d "
                                "500Hz:%d "
                                "1kHz:%d "
                                "2kHz:%d "
                                "4kHz:%d "
                                "8kHz:%d "
                                "16kHz:%d",
                                eq_params[0], eq_params[1], eq_params[2], eq_params[3], eq_params[4],
                                eq_params[5], eq_params[6], eq_params[7], eq_params[8], eq_params[9]);
                            
                            app_gatt_uart_send_data((uint8_t *)response, strlen(response));
                            ESP_LOGI(TAG, "EQ parameters sent successfully %s", response);
                        } else {
                            ESP_LOGE(TAG, "EQ parameter count error, need 10 parameters, received %d", param_count);
                            char error_msg[100];
                            snprintf(error_msg, sizeof(error_msg), 
                                "set eq count error, need 10 parameters, received %d", param_count);
                            app_gatt_uart_send_data((uint8_t *)error_msg, strlen(error_msg));
                        }
                        
                        free(data);
                    }

                    // 添加读取EQ参数的处理
                    else if (param->write.len == 5 && strncmp((char*)param->write.value, "GETEQ", 5) == 0) {
                        const int *eq_params = app_player_get_eq();
                        
                        char response[256];
                        snprintf(response, sizeof(response), 
                            "Current EQ "
                            "31Hz:%d "
                            "62Hz:%d "
                            "125Hz:%d "
                            "250Hz:%d "
                            "500Hz:%d "
                            "1kHz:%d "
                            "2kHz:%d "
                            "4kHz:%d "
                            "8kHz:%d "
                            "16kHz:%d",
                            eq_params[0], eq_params[1], eq_params[2], eq_params[3], eq_params[4],
                            eq_params[5], eq_params[6], eq_params[7], eq_params[8], eq_params[9]);
                        
                        app_gatt_uart_send_data((uint8_t *)response, strlen(response));
                    }
                }
                if (param->write.handle == service_handle_table[UART_SERVICE_IDX_TX_CFG]) {
                    memcpy(uart_tx_ccc, param->write.value, sizeof(uart_tx_ccc));
                }
                if (param->write.need_rsp) {
                    esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id, ESP_GATT_OK, NULL);
                }
            }
            break;

        case ESP_GATTS_CONNECT_EVT:
            conn_id = param->connect.conn_id;
            uart_gatts_if = gatts_if;
            break;

        case ESP_GATTS_DISCONNECT_EVT:
            conn_id = 0xffff;
            uart_gatts_if = 0xff;
            break;

        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (param->add_attr_tab.status != ESP_GATT_OK) {
                ESP_LOGE(TAG, "create attribute table failed");
            } else {
                memcpy(service_handle_table, param->add_attr_tab.handles, sizeof(service_handle_table));
                esp_ble_gatts_start_service(service_handle_table[UART_SERVICE_IDX_SVC]);
            }
            break;

        default:
            break;
    }
}

esp_err_t app_gatt_uart_send_data(uint8_t *data, uint16_t len)
{
    if (conn_id == 0xffff || uart_gatts_if == 0xff || uart_tx_ccc[0] != 0x01) {
        return ESP_FAIL;
    }
    
    return esp_ble_gatts_send_indicate(uart_gatts_if, conn_id, 
                                     service_handle_table[UART_SERVICE_IDX_TX_VAL],
                                     len, data, false);
}
