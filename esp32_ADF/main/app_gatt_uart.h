#ifndef _APP_GATT_UART_H_
#define _APP_GATT_UART_H_

#include "esp_gatts_api.h"

void app_gatts_uart_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
esp_err_t app_gatt_uart_send_data(uint8_t *data, uint16_t len);

#endif
