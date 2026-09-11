#ifndef _APP_GATT_DEVICE_H_
#define _APP_GATT_DEVICE_H_
#include "esp_gatts_api.h"


void app_gatt_device_set_battery_level(int level);
void app_gatt_device_set_firmware_version(char * version);
void app_gatts_device_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
#endif // _APP_GATT_DEVICE_H_