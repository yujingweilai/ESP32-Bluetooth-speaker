#ifndef _APP_BLE_H_
#define _APP_BLE_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GATTS_CHAR_VAL_LEN_MAX      500
#define CHAR_DECLARATION_SIZE       (sizeof(uint8_t))
#define PREPARE_BUF_MAX_SIZE        1024
typedef struct {
    uint8_t                 *prepare_buf;
    int                     prepare_len;
} prepare_type_env_t;

esp_err_t app_ble_init(void);
bool app_ble_is_connected(void);
enum {
    GATT_PROFILE_ID_DEVICE,
    GATT_PROFILE_ID_OTA,
    GATT_PROFILE_ID_UART,
    GATT_PROFILE_NUM
};
// #define DEFAULT_DEVICE_NAME_BLE "VIB_PLAYER_BLE"
#endif // _APP_BLE_H_