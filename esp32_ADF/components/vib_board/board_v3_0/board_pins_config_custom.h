#ifndef _BOARD_PINS_CONFIG_CUSTOM_H_
#define _BOARD_PINS_CONFIG_CUSTOM_H_


/**
 * @brief Get the gpio number for battery detect enable
 *
 * @return  -1      non-existent
 *          Others  gpio number
 */
int8_t get_battery_detect_enable_pin(void);

/**
 * @brief Get the gpio number for battery charge enable
 *
 * @return  -1      non-existent
 *          Others  gpio number
 */
int8_t get_battery_charge_in_pin(void);

/**
 * @brief Get the gpio number for battery charge status
 *
 * @return  -1      non-existent
 *          Others  gpio number
 */
uint8_t get_battery_adc_channel(void);



#endif // _BOARD_PINS_CONFIG_CUSTOM_H_