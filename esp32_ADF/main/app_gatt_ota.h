#ifndef _APP_GATT_OTA_H_
#define _APP_GATT_OTA_H_


void app_gatt_ota_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);

#endif // _APP_GATT_OTA_H_