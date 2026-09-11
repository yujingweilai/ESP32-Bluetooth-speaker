#include <string.h>
#include "charger.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bq256xx_drv.h"
#include "bq256xx.h" 

static const char *TAG = "cahrger";
#define CHARGER_VERBOSE_LOG_ENABLE 0

static struct
{
    bq256xx_t *dev;
    TaskHandle_t task;
    int wdt_ms;
    int last_charge_state;    
    TickType_t last_wdt;
} s_instance, *s = NULL;


esp_err_t chg_get_snap(chg_snapshot_t *snap)
{

    uint8_t pn = 0;
    uint16_t u16 = 0;
    

    memset(snap, 0, sizeof(chg_snapshot_t));

    snap->present = (bq256xx_check_presence(s->dev, &pn) == ESP_OK);
    
    if (bq256xx_read_reg_u16(s->dev, BQ256XX_VBAT_ADC, &u16) == ESP_OK)
    {
        float v = (float)((u16 >> 1) & 0x1FFF) * 1.99f;
        snap->vbat_mV = v;
    }

    if (bq256xx_read_reg_u16(s->dev, BQ256XX_IBAT_ADC, &u16) == ESP_OK)
    {
        int16_t raw = (int16_t)((u16 >> 2) & 0x3FFF);
        if (raw & 0x2000)
            raw = -(int16_t)((~raw + 1) & 0x3FFF);
        snap->ibat_mA = (float)(raw * 4);
    }

    if (bq256xx_read_reg_u16(s->dev, BQ256XX_VSYS_ADC, &u16) == ESP_OK)
    {
        float vsys = (float)((u16 >> 1) & 0x1FFF) * 1.99f;
        snap->vsys_mV = vsys;
    }

    if (bq256xx_read_reg_u16(s->dev, BQ256XX_VBUS_ADC, &u16) == ESP_OK)
    {
        float vbus = (float)((u16 >> 1) & 0x1FFF) * 1.99f;
        snap->vbus_mV = vbus;
    }

    if (bq256xx_read_reg_u16(s->dev, BQ256XX_TS_ADC, &u16) == ESP_OK)
    {
        uint16_t raw12 = (uint16_t)(u16 & 0x0FFF);
        snap->ts_percent = (float)raw12 * (100.0f / 4095.0f);
    }

    bool charging = false;
    uint8_t st0 = 0, st1 = 0;
    if (bq256xx_read_reg(s->dev, BQ256XX_CHARGER_STATUS_0, &st0) == ESP_OK &&
        bq256xx_read_reg(s->dev, BQ256XX_CHARGER_STATUS_1, &st1) == ESP_OK)
    {
        const uint8_t vbus_stat = (st1 & 0x07);
        const uint8_t chg_stat = (st1 >> 3) & 0x03;
        snap->vbus_stat = vbus_stat;
        snap->chg_stat = chg_stat;

        if (chg_stat == 1 || chg_stat == 2)
        {
            charging = true;
        }
        else if (chg_stat == 3 && snap->ibat_mA > 20.0f)
        {
            charging = true;
        }

        if (vbus_stat == 0)
        {
            charging = false;
        }
    }
    else
    {
        if (snap->ibat_mA > 30.0f)
        {
            charging = true;
        }
    }

    if (!snap->present)
        charging = false;

    snap->charging = charging;

    /* 读取 TS 温度状态 (来自 REG0x1F FAULT_STATUS, Bits 2:0) */
    uint8_t flt = 0;
    if (bq256xx_read_reg(s->dev, BQ256XX_FAULT_STATUS, &flt) == ESP_OK)
    {
        snap->ts_stat = flt & BQ256XX_TS_STAT_MASK;  /* Bits 2:0 */
    }

    if ((xTaskGetTickCount() - s->last_wdt) >= pdMS_TO_TICKS(s->wdt_ms))
    {
        bq256xx_kick_watchdog(s->dev);
        s->last_wdt = xTaskGetTickCount();
    }
    return ESP_OK;
}

void chg_dump_debug(void)
{
    if (!s->dev)
        return;
    uint8_t c0 = 0, c1 = 0, c2 = 0, c3 = 0, tmr = 0, ntc0 = 0, ntc1 = 0, ntc2 = 0, st0 = 0, st1 = 0, flt = 0, flg0 = 0, flg1 = 0, f0 = 0, msk0 = 0, msk1 = 0, adc = 0, afd0 = 0;
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_CONTROL_0, &c0);      // 0x16
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_CONTROL_1, &c1);      // 0x17
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_CONTROL_2, &c2);      // 0x18
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_CONTROL_3, &c3);      // 0x19
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_TIMER_CONTROL, &tmr); // 0x15
    bq256xx_read_reg(s->dev, BQ256XX_NTC_CONTROL_0, &ntc0);        // 0x1A
    bq256xx_read_reg(s->dev, BQ256XX_NTC_CONTROL_1, &ntc1);        // 0x1B
    bq256xx_read_reg(s->dev, BQ256XX_NTC_CONTROL_2, &ntc2);        // 0x1C
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_STATUS_0, &st0);      // 0x1D
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_STATUS_1, &st1);      // 0x1E
    bq256xx_read_reg(s->dev, BQ256XX_FAULT_STATUS, &flt);          // 0x1F
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_FLAG_0, &flg0);       // 0x20
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_FLAG_1, &flg1);       // 0x21
    bq256xx_read_reg(s->dev, BQ256XX_FAULT_0, &f0);                // 0x22
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_MASK_0, &msk0);       // 0x23
    bq256xx_read_reg(s->dev, BQ256XX_CHARGER_MASK_1, &msk1);       // 0x24
    bq256xx_read_reg(s->dev, BQ256XX_ADC_CONTROL, &adc);           // 0x26
    bq256xx_read_reg(s->dev, BQ256XX_ADC_FUNC_DISABLE_0, &afd0);   // 0x27

    uint16_t reg02_03 = 0; // CHARGE_CURRENT_LIMIT (ICHG)
    uint16_t reg04_05 = 0; // CHARGE_VOLTAGE_LIMIT (VBATREG)
    uint16_t reg06_07 = 0; // INPUT_CURRENT_LIMIT (IINLIM code split)
    uint16_t reg08_09 = 0; // INPUT_VOLTAGE_LIMIT (VINDPM code split)
    uint8_t reg10 = 0;     // PRE_CHG_CONTROL (IPRECHG)
    bq256xx_read_reg_u16(s->dev, BQ256XX_CHARGE_CURRENT_LIMIT, &reg02_03);
    bq256xx_read_reg_u16(s->dev, BQ256XX_CHARGE_VOLTAGE_LIMIT, &reg04_05);
    bq256xx_read_reg_u16(s->dev, BQ256XX_INPUT_CURRENT_LIMIT, &reg06_07);
    bq256xx_read_reg_u16(s->dev, BQ256XX_INPUT_VOLTAGE_LIMIT, &reg08_09);
    bq256xx_read_reg(s->dev, BQ256XX_PRE_CHG_CONTROL, &reg10);

    ESP_LOGI(TAG,
             "DBG: C0=%02X C1=%02X C2=%02X C3=%02X TMR=%02X NTC0=%02X NTC1=%02X NTC2=%02X "
             "ST0=%02X ST1=%02X FAULT=%02X FLAG0=%02X FLAG1=%02X F0=%02X MSK0=%02X MSK1=%02X "
             "ADC=%02X AFD0=%02X | 02-03=%04X 04-05=%04X 06-07=%04X 08-09=%04X 10=%02X",
             c0, c1, c2, c3, tmr, ntc0, ntc1, ntc2, st0, st1, flt, flg0, flg1, f0, msk0, msk1, adc, afd0,
             reg02_03, reg04_05, reg06_07, reg08_09, reg10);

    const bool en_hiz = (c0 & BQ256XX_C0_EN_HIZ) != 0;
    const bool en_chg = (c0 & BQ256XX_C0_EN_CHG) != 0;
    const uint8_t wdt = (c0 & BQ256XX_C0_WDT_MASK); // 0=off,1=50s,2=100s,3=200s
    const bool en_extilim = (c3 & BQ256XX_C3_EN_EXTILIM) != 0;
    const bool adc_en = (adc & 0x80) != 0;
    const uint8_t adc_sample = (adc >> 4) & 0x3;
    const bool adc_avg = (adc & 0x08) != 0;
    const bool ts_adc_dis = (afd0 & 0x04) != 0;
    const bool ts_ignore = (ntc0 & 0x80) != 0;
    const bool stat_wd = (st0 & BQ256XX_WD_STAT_MASK) != 0;
    const bool stat_tmr = (st0 & BQ256XX_SAFETY_TMR_STAT_MASK) != 0;
    const bool stat_vindpm = (st0 & BQ256XX_VINDPM_STAT_MASK) != 0;
    const bool stat_iindpm = (st0 & BQ256XX_IINDPM_STAT_MASK) != 0;
    const bool stat_treg = (st0 & BQ256XX_TREG_STAT_MASK) != 0;
    const bool stat_vsys = (st0 & BQ256XX_VSYS_STAT_MASK) != 0;

    const uint8_t vbus_stat = (st1 & 0x07);     // 0=no vbus, >0 present
    const uint8_t chg_stat = (st1 >> 3) & 0x03; // 0=idle/term,1=pre/CC,2=CV,3=topoff
    const uint8_t ts_stat = (st1 >> 5) & 0x07;  // TS zone/bias status (when TS enabled)
    uint16_t v02_03 = reg02_03;
    uint16_t ichg_code = (uint16_t)((v02_03 >> 5) & 0x3F);
    int ichg_uA = (int)ichg_code * BQ256XX_ICHG_STEP_uA;
    uint16_t v04_05 = reg04_05;
    uint16_t vreg_code = (uint16_t)((v04_05 & BQ256XX_VBATREG_MASK) >> BQ256XX_VBATREG_SHIFT);
    int vbatreg_uV = BQ2560X_VBATREG_OFFSET_uV + (int)vreg_code * BQ2560X_VBATREG_STEP_uV;
    uint8_t r06 = (uint8_t)(reg06_07 & 0xFF);
    uint8_t r07 = (uint8_t)(reg06_07 >> 8);
    int iin_code = ((r06 >> 4) & 0x0F) | ((r07 & 0x0F) << 4);
    int iinlim_uA = BQ256XX_IINDPM_OFFSET_uA + iin_code * BQ256XX_IINDPM_STEP_uA;
    uint16_t packed = reg08_09;
    int vindpm_code = ((packed >> 5) & 0x07) | ((packed >> 8) << 3);
    int vindpm_uV = vindpm_code * BQ256XX_VINDPM_STEP_uV;
    int ipre_code = (reg10 >> 3) & 0x1F;
    int ipre_uA = ipre_code * BQ256XX_IPRECHG_STEP_uA;
    uint16_t ts = 0, vb = 0, vs = 0, ibat = 0, vbus = 0, ibus = 0, tdie = 0;
    bq256xx_read_reg_u16(s->dev, BQ256XX_TS_ADC, &ts);
    bq256xx_read_reg_u16(s->dev, BQ256XX_VBAT_ADC, &vb);
    bq256xx_read_reg_u16(s->dev, BQ256XX_VSYS_ADC, &vs);
    bq256xx_read_reg_u16(s->dev, BQ256XX_IBAT_ADC, &ibat);
    bq256xx_read_reg_u16(s->dev, BQ256XX_VBUS_ADC, &vbus);
    bq256xx_read_reg_u16(s->dev, BQ256XX_IBUS_ADC, &ibus);
    bq256xx_read_reg_u16(s->dev, BQ256XX_TDIE_ADC, &tdie);
    // float ts_pct = (float)(ts & 0x0FFF) * (100.0f / 4095.0f);
    float ts_pct = (float)(ts & 0x0FFF) * 0.0961f;
    float vbat_mV = (float)((vb >> 1) & 0x1FFF) * 1.99f;
    float vsys_mV = (float)((vs >> 1) & 0x1FFF) * 1.99f;
    int16_t ichg_raw = (int16_t)((ibat >> 2) & 0x3FFF);
    if (ichg_raw & 0x2000)
        ichg_raw = -(int16_t)((~ichg_raw + 1) & 0x3FFF);
    float ibat_mA = (float)ichg_raw * 4.0f;

    ESP_LOGI(TAG,
             "CTRL: HIZ=%d EN_CHG=%d WDT=%u | EXT_ILIM=%d | TMR{SAFE=%d,2x=%d,PRE=%d} | TS_IGNORE=%d "
             "| ADC{EN=%d,SAMPLE=%u,AVG=%d,TS_ADC_DIS=%d}",
             en_hiz, en_chg, (unsigned)wdt,
             en_extilim,
             (tmr & BQ256XX_TMR_EN_SAFETY) ? 1 : 0,
             (tmr & BQ256XX_TMR_TMR2X_EN) ? 1 : 0,
             (tmr & BQ256XX_TMR_PRECHG_TMR) ? 1 : 0,
             ts_ignore,
             adc_en, (unsigned)adc_sample, adc_avg, ts_adc_dis);

    ESP_LOGI(TAG,
             "LIMITS: ICHG=%dmA VBATREG=%.2fV | IINLIM=%dmA (EXT_ILIM=%d) | VINDPM=%.2fV | IPRE=%dmA",
             ichg_uA / 1000, vbatreg_uV / 1e6f,
             iinlim_uA / 1000, en_extilim, vindpm_uV / 1e6f, ipre_uA / 1000);

    ESP_LOGI(TAG,
             "STATUS: VBUS_STAT=%u CHG_STAT=%u TS_STAT=0x%X | DPM{V=%d,I=%d} VSYS_REG=%d TREG=%d | WD_ACT=%d TMR_ACT=%d | FAULT=%02X",
             vbus_stat, chg_stat, ts_stat, stat_vindpm, stat_iindpm, stat_vsys, stat_treg, stat_wd, stat_tmr, flt);

    ESP_LOGI(TAG,
             "ADC: VBAT=%.0fmV VSYS=%.0fmV IBAT=%.0fmA | VBUS_RAW=0x%04X IBUS_RAW=0x%04X | TS=%.1f%% TDIE_RAW=0x%04X",
             vbat_mV, vsys_mV, ibat_mA, vbus, ibus, ts_pct, tdie);

    const char *why =
        en_hiz ? "HIZ=1 (turn off HIZ)" : (vbus_stat == 0 ? "No VBUS" : stat_vindpm ? "VINDPM limiting (raise VINDPM or supply)"
                                                                    : stat_iindpm   ? "IINDPM limiting (raise IINLIM / adapter)"
                                                                    : stat_tmr      ? "Safety timer active/expired"
                                                                    : (!en_chg)     ? "EN_CHG=0"
                                                                                    : "OK/charging");

    ESP_LOGW(TAG, "DIAG: %s | CHG_STAT=%u IPRE=%dmA ICHG=%dmA IINLIM=%dmA VINDPM=%.2fV",
             why, chg_stat, ipre_uA / 1000, ichg_uA / 1000, iinlim_uA / 1000, vindpm_uV / 1e6f);
}

int chg_state_change_notify(void)
{
    if (!s->dev)
    {
        return -1;
    }
    uint8_t st0 = 0;
    uint8_t st1 = 0;
    if (bq256xx_read_reg(s->dev, BQ256XX_CHARGER_STATUS_0, &st0) != ESP_OK)
    {
        return -1;
    }
    if (bq256xx_read_reg(s->dev, BQ256XX_CHARGER_STATUS_1, &st1) != ESP_OK)
    {
        return -1;
    }
    uint8_t chrg_stat = (st1 >> 3) & 0x03;
    uint8_t vbus_stat = st1 & 0x07;
    bool indpm = (st0 & (BQ256XX_VINDPM_STAT_MASK | BQ256XX_IINDPM_STAT_MASK)) != 0;
    bool vbus_present = (vbus_stat != 0) || (chrg_stat != 0) || indpm;
    int should_charge = vbus_present ? 1 : 0;
    if (should_charge == s->last_charge_state)
    {
        return should_charge;
    }
    if (should_charge)
    {
        (void)bq256xx_set_hiz(s->dev, false);
        (void)bq256xx_set_charge_enable(s->dev, true);
#if CHARGER_VERBOSE_LOG_ENABLE
        chg_dump_debug();
#endif
        ESP_LOGI(TAG,
                 "[charge] VBUS detected, enable charge, vbus_stat=%u chg_stat=%u indpm=%d",
                 (unsigned)vbus_stat,
                 (unsigned)chrg_stat,
                 indpm ? 1 : 0);
    }
    else
    {
        ESP_LOGW(TAG, "[charge] VBUS lost, disable charge, vbus_stat=%u", (unsigned)vbus_stat);
        (void)bq256xx_set_charge_enable(s->dev, false);
        (void)bq256xx_set_hiz(s->dev, true);
    }
    s->last_charge_state = should_charge;
    return should_charge;
}


esp_err_t chg_init(const chg_service_cfg_t *cfg)
{
    if (!cfg)
        return ESP_ERR_INVALID_ARG;
    
    if (s)
        return ESP_OK;
    
    s = &s_instance;
    memset(s, 0, sizeof(*s));
    s->wdt_ms = cfg->wdt_kick_ms ? cfg->wdt_kick_ms : 3000;
    s->last_charge_state = -1;
    esp_err_t err;
    err = bq256xx_create(cfg->i2c_dev, &s->dev);
    if( err!= ESP_OK){
        s = NULL;
        return err;
    }

    bq256xx_enable_adc(s->dev, true);
    (void)bq256xx_set_watchdog(s->dev, BQ_WDT_200S);
    (void)bq256xx_set_safety_timers(s->dev, true, 10, 90);
    /***************************************************************************
     * NTC/JEITA 温度保护配置 (NTC/JEITA Temperature Protection Configuration)
     * 
     * 硬件电路: R_T1 = 5.23kΩ (上拉), R_T2 = 30.1kΩ (并联), 103AT NTC 热敏电阻
     * 
     * 配置策略:
     * - 启用 NTC 温度监测 (TS_IGNORE = 0)
     * - Cold 区间 (<0°C): 停止充电
     * - Cool 区间 (0-10°C): 电流降至 20%
     * - Normal 区间 (10-45°C): 正常充电
     * - Warm 区间 (45-60°C): 40% ICHG 限流 (TS_ISET_WARM = 10b)
     * - Hot 区间 (>60°C): 停止充电
     **************************************************************************/
    
    (void)bq256xx_set_ts_ignore(s->dev, false);   /* TS_IGNORE=0: 启用 NTC 温度保护 */
    (void)bq256xx_enable_ts_adc(s->dev, true);    /* 开启 TS ADC, TS% 随温度更新 */
    
    /* TS_ISET_WARM(0x1A[3:2]): 默认 00=停止。改为 10=40% ICHG, 使 45~60°C 限流而非停 */
    (void)bq256xx_update_bits(s->dev, BQ256XX_NTC_CONTROL_0, 0x0C, 0x08);
    (void)bq256xx_set_treg_120(s->dev);  /* 结温调节 120°C, 避免 42°C 环境即因 60°C 结温而限流 */

    /* 写入 NTC 控制寄存器，启用 JEITA 温度保护 */
    (void)bq256xx_write_reg(s->dev, BQ256XX_NTC_CONTROL_1, BQ256XX_NTC1_CFG_DEFAULT);       /* 0x25 */
    (void)bq256xx_write_reg(s->dev, BQ256XX_NTC_CONTROL_2, BQ256XX_NTC2_CFG_DEFAULT);       /* 0x3F */

    ESP_LOGI(TAG, "NTC/JEITA config: NTC0=0x08(Warm=40%% ICHG), NTC1=0x%02X, NTC2=0x%02X, TREG=120C",
             BQ256XX_NTC1_CFG_DEFAULT, BQ256XX_NTC2_CFG_DEFAULT);
    (void)bq256xx_enable_ilim_pin(s->dev, false);
    (void)bq256xx_set_vindpm_uV(s->dev, 4200000);
    (void)bq256xx_set_iinlim_uA(s->dev, 1500000);
    (void)bq256xx_set_vbatreg_uV(s->dev, 4200000);
    (void)bq256xx_set_ichg_uA(s->dev, 1000000);
    (void)bq256xx_set_iterm_uA(s->dev, 100000);
    (void)bq256xx_set_iprechg_uA(s->dev, 190000);
    (void)bq256xx_enable_adc(s->dev, true);
    bq256xx_kick_watchdog(s->dev);
    s->last_wdt = xTaskGetTickCount();
    chg_state_change_notify();
    return ESP_OK;
}
