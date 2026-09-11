#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "bq256xx_drv.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        i2c_master_dev_handle_t i2c_dev;  /*!< I2C master device handle for BQ256xx */
        int wdt_kick_ms;
    } chg_service_cfg_t;

    typedef struct
    {
        bool present;
        float vbat_mV;
        float ibat_mA;
        float vsys_mV;
        float vbus_mV;
        float ts_percent;       /* TS ADC 电压百分比 (0-100%) */
        uint8_t ts_stat;        /* TS temperature status (REG0x1F FAULT_STATUS):
                                 * 0 = Normal (TS_NORMAL)
                                 * 1 = Cold (TS_COLD)
                                 * 2 = Hot (TS_HOT)
                                 * 3 = Cool (TS_COOL)
                                 * 4 = Warm (TS_WARM)
                                 * 5 = Precool (TS_PRECOOL)
                                 * 6 = Prewarm (TS_PREWARM)
                                 * 7 = Fault (TS pin bias fault) */
        uint8_t vbus_stat;      /* REG0x1E bits[2:0] */
        uint8_t chg_stat;       /* REG0x1E bits[4:3]: 0=disable/term,1=CC,2=CV,3=topoff */
        bool charging;
    } chg_snapshot_t;

    
    int chg_state_change_notify(void);
    esp_err_t chg_init(const chg_service_cfg_t *cfg);
    esp_err_t chg_get_snap(chg_snapshot_t *snap);
    void chg_dump_debug(void);
    
    
    void chg_deinit(void);
    void chg_notify_irq_cb(void);
#ifdef __cplusplus
}
#endif
