#ifndef _APP_BT_H_
#define _APP_BT_H_

#ifdef __cplusplus
extern "C" {
#endif
#define DEFAULT_DEVICE_NAME "OOHM"
#include "esp_peripherals.h"
#include "esp_gap_bt_api.h"



/**
 * @brief          Initializes the bluetooth
 *
 * @param[in]      set           the specific peripheral set handle
 *
 * @return         esp_periph_handle_t      the bt peripheral handle
 *                 NULL                     failed
 */
esp_periph_handle_t app_bt_init(esp_periph_set_handle_t set);

/**
 * @brief          Deinit the bluetooth
 *
 */
void app_bt_deinit(void);

/**
 * @brief          Enable a2dp function
 *
 * @return         ESP_OK      success
 *                 ESP_FAIL    failed
 */
esp_err_t app_bt_dicoverable(void);
esp_err_t app_bt_non_discoverable(void);

/**
 * @brief          Disable a2dp function
 *
 * @param[in]      bda                      remote bt adress (mac address)
 *
 * @return         ESP_OK      success
 *                 ESP_FAIL    failed
 */
esp_err_t app_bt_stop(void);


/**
 * @brief          Start bt
 *
 * @return         ESP_OK      success
 *                 ESP_FAIL    failed
 */
esp_err_t app_bt_start();
esp_err_t app_bt_start_or_stop();
/**
 * @brief          Set bt bda
 *
 * @param[in]      addr  remote bt adress (mac address)
 */
void app_bt_set_addr(esp_bd_addr_t *addr);

esp_periph_handle_t app_bt_get_periph();
bool app_bt_get_connectable();

esp_bd_addr_t* app_bt_get_last_connected_bdaddr();


esp_err_t app_bt_recovery();

#ifdef __cplusplus
}
#endif

#endif // _APP_BT_H_