#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/i2c_master.h"
#include "bq256xx.h"

#ifdef __cplusplus
extern "C"
{
#endif

#ifndef BQ256XX_VERIFY_WRITES
#define BQ256XX_VERIFY_WRITES 1
#endif

#ifndef BQ256XX_WRITE_RETRIES
#define BQ256XX_WRITE_RETRIES 1
#endif


    typedef struct bq256xx_dev bq256xx_t;


    esp_err_t bq256xx_create(i2c_master_dev_handle_t i2c_dev, bq256xx_t **dev);
    void bq256xx_destroy(bq256xx_t *dev);
    esp_err_t bq256xx_read_reg(const bq256xx_t *dev, uint8_t reg, uint8_t *val);
    esp_err_t bq256xx_write_reg(const bq256xx_t *dev, uint8_t reg, uint8_t val);
    esp_err_t bq256xx_update_bits(const bq256xx_t *dev, uint8_t reg, uint8_t mask, uint8_t set);
    esp_err_t bq256xx_update_bits_u16(const bq256xx_t *dev, uint8_t reg_lsb, uint16_t mask16, uint16_t set16);
    esp_err_t bq256xx_read_reg_u16(const bq256xx_t *dev, uint8_t reg_lsb, uint16_t *val);
    esp_err_t bq256xx_write_reg_u16(const bq256xx_t *dev, uint8_t reg_lsb, uint16_t val);
    esp_err_t bq256xx_set_vbatreg_uV(const bq256xx_t *dev, int vbat_uV);
    esp_err_t bq256xx_set_vindpm_uV(const bq256xx_t *dev, int vindpm_uV);
    esp_err_t bq256xx_set_ichg_uA(const bq256xx_t *dev, int ichg_uA);
    esp_err_t bq256xx_set_iterm_uA(const bq256xx_t *dev, int iterm_uA);
    esp_err_t bq256xx_set_iinlim_uA(const bq256xx_t *dev, int iin_uA);
    esp_err_t bq256xx_set_iprechg_uA(const bq256xx_t *dev, int iprech_uA);
    esp_err_t bq256xx_enable_ilim_pin(const bq256xx_t *dev, bool enable);
    esp_err_t bq256xx_kick_watchdog(bq256xx_t *dev);
    esp_err_t bq256xx_enable_adc(const bq256xx_t *dev, bool en);
    esp_err_t bq256xx_enable_ts_adc(const bq256xx_t *dev, bool en);
    esp_err_t bq256xx_check_presence(const bq256xx_t *dev, uint8_t *part_num);
    void bq256xx_notify_int(bq256xx_t *dev);
    esp_err_t bq256xx_set_charge_enable(const bq256xx_t *dev, bool enable);
    esp_err_t bq256xx_enter_ship_mode(const bq256xx_t *dev);
    esp_err_t bq256xx_set_hiz(const bq256xx_t *dev, bool enable);
    esp_err_t bq256xx_set_safety_timers(const bq256xx_t *dev, bool enable, int fastcharge_hours, int precharge_minutes);
    esp_err_t bq256xx_set_ts_ignore(const bq256xx_t *dev, bool ignore);
    esp_err_t bq256xx_set_treg_120(const bq256xx_t *dev);

    typedef enum
    {
        BQ_WDT_DIS = 0,
        BQ_WDT_50S = 1,
        BQ_WDT_100S = 2,
        BQ_WDT_200S = 3
    } bq256xx_wdt_t;
    esp_err_t bq256xx_set_watchdog(const bq256xx_t *dev, bq256xx_wdt_t wdt);
    

#ifdef __cplusplus
}
#endif
