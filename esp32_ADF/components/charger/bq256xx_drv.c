#include <string.h>
#include "bq256xx_drv.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"

struct bq256xx_dev
{
    i2c_master_dev_handle_t i2c_dev;
    SemaphoreHandle_t lock;
    volatile bool int_flag;
};

static const char *TAG = "bq256xx_drv";

static inline uint8_t reg_verify_mask(uint8_t reg)
{
    switch (reg)
    {
    case BQ256XX_CHARGER_CONTROL_0:
        return (uint8_t)(0xFFu & ~BQ256XX_C0_WD_RST);
    default:
        return 0xFFu;
    }
}

static esp_err_t write_reg_verify_masked(const bq256xx_t *dev, uint8_t reg, uint8_t v, uint8_t verify_mask)
{
    esp_err_t e = ESP_OK;
    uint8_t buf[2] = {reg, v};
    for (int attempt = 0; attempt <= BQ256XX_WRITE_RETRIES; ++attempt)
    {
        e = i2c_master_transmit(dev->i2c_dev, buf, sizeof(buf), 50);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C write failed reg 0x%02X err=%s (attempt %d)", reg, esp_err_to_name(e), attempt);
            break;
        }
#if BQ256XX_VERIFY_WRITES
        uint8_t rd = 0;
        uint8_t wreg = reg;
        e = i2c_master_transmit_receive(dev->i2c_dev, &wreg, 1, &rd, 1, 50);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C readback failed reg 0x%02X err=%s (attempt %d)", reg, esp_err_to_name(e), attempt);
            break;
        }
        if (((rd ^ v) & verify_mask) == 0)
            break; // success
        ESP_LOGW(TAG, "Verify mismatch reg 0x%02X mask 0x%02X: wrote 0x%02X, read 0x%02X (attempt %d)",
                 reg, verify_mask, v, rd, attempt);
        if (attempt == BQ256XX_WRITE_RETRIES)
            e = ESP_ERR_INVALID_RESPONSE;
#else
        break;
#endif
    }
    return e;
}

static inline void lock(bq256xx_t *d)
{
    if (d->lock)
        xSemaphoreTake(d->lock, portMAX_DELAY);
}

static inline void unlock(bq256xx_t *d)
{
    if (d->lock)
        xSemaphoreGive(d->lock);
}

static bq256xx_t s_dev;

esp_err_t  bq256xx_create(i2c_master_dev_handle_t i2c_dev, bq256xx_t **out)
{
    if (!i2c_dev || !out)
        return ESP_ERR_INVALID_ARG;

    bq256xx_t *d = &s_dev;
    if (!d->lock) {
        d->lock = xSemaphoreCreateMutex();
        if (!d->lock)
        {
            return ESP_ERR_NO_MEM;
        }
    }
    d->i2c_dev = i2c_dev;
    *out = d;
    return ESP_OK;
}

void bq256xx_destroy(bq256xx_t *d)
{
    if (!d)
        return;
    if (d->lock) {
        vSemaphoreDelete(d->lock);
        d->lock = NULL;
    }
}

esp_err_t bq256xx_read_reg(const bq256xx_t *dev, uint8_t reg, uint8_t *val)
{
    if (!dev || !val)
        return ESP_ERR_INVALID_ARG;
    lock((bq256xx_t *)dev);
    esp_err_t e = i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, val, 1, 50);
    unlock((bq256xx_t *)dev);
    return e;
}

esp_err_t bq256xx_write_reg(const bq256xx_t *dev, uint8_t reg, uint8_t v)
{
    if (!dev)
        return ESP_ERR_INVALID_ARG;
    esp_err_t e = ESP_OK;
    lock((bq256xx_t *)dev);
    e = write_reg_verify_masked(dev, reg, v, reg_verify_mask(reg));
    unlock((bq256xx_t *)dev);
    return e;
}

esp_err_t bq256xx_update_bits(const bq256xx_t *dev, uint8_t reg, uint8_t mask, uint8_t set)
{

    uint8_t cur = 0;
    esp_err_t e = bq256xx_read_reg(dev, reg, &cur);
    if (e != ESP_OK)
        return e;
    uint8_t next = (cur & ~mask) | (set & mask);
    ESP_LOGD(TAG, "Update reg 0x%02X: cur 0x%02X mask 0x%02X set 0x%02X -> 0x%02X", reg, cur, mask, set, next);
    if (next == cur)
        return ESP_OK;
    lock((bq256xx_t *)dev);
    e = write_reg_verify_masked(dev, reg, next, mask);
    unlock((bq256xx_t *)dev);
    return e;
}

esp_err_t bq256xx_update_bits_u16(const bq256xx_t *dev, uint8_t reg_lsb, uint16_t mask16, uint16_t set16)
{
    uint16_t cur = 0;
    ESP_RETURN_ON_ERROR(bq256xx_read_reg_u16(dev, reg_lsb, &cur), "bq256xx", "read16");
    uint16_t next = (cur & ~mask16) | (set16 & mask16);
    if (next == cur)
        return ESP_OK;
    return bq256xx_write_reg_u16(dev, reg_lsb, next);
}

esp_err_t bq256xx_read_reg_u16(const bq256xx_t *dev, uint8_t reg_lsb, uint16_t *val)
{
    if (!dev || !val)
        return ESP_ERR_INVALID_ARG;
    uint8_t w = reg_lsb, r[2] = {0};
    lock((bq256xx_t *)dev);
    esp_err_t e = i2c_master_transmit_receive(dev->i2c_dev, &w, 1, r, 2, 50);
    unlock((bq256xx_t *)dev);
    if (e == ESP_OK)
        *val = (uint16_t)r[0] | ((uint16_t)r[1] << 8);
    return e;
}

esp_err_t bq256xx_write_reg_u16(const bq256xx_t *dev, uint8_t reg_lsb, uint16_t val)
{
    if (!dev)
        return ESP_ERR_INVALID_ARG;
    uint8_t buf[3] = {reg_lsb, (uint8_t)(val & 0xFF), (uint8_t)(val >> 8)};
    esp_err_t e = ESP_OK;
    lock((bq256xx_t *)dev);
    for (int attempt = 0; attempt <= BQ256XX_WRITE_RETRIES; ++attempt)
    {
        e = i2c_master_transmit(dev->i2c_dev, buf, sizeof(buf), 50);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C write failed regs 0x%02X-0x%02X err=%s (attempt %d)", reg_lsb, (uint8_t)(reg_lsb + 1), esp_err_to_name(e), attempt);
            break;
        }
#if BQ256XX_VERIFY_WRITES
        uint8_t reg = reg_lsb;
        uint8_t rd[2] = {0};
        e = i2c_master_transmit_receive(dev->i2c_dev, &reg, 1, rd, 2, 50);
        if (e != ESP_OK)
        {
            ESP_LOGE(TAG, "I2C readback failed regs 0x%02X-0x%02X err=%s (attempt %d)", reg_lsb, (uint8_t)(reg_lsb + 1), esp_err_to_name(e), attempt);
            break;
        }
        uint16_t rcv = (uint16_t)rd[0] | ((uint16_t)rd[1] << 8);
        if (rcv == val)
            break; // success
        ESP_LOGW(TAG, "Write verify mismatch regs 0x%02X-0x%02X: wrote 0x%04X, read 0x%04X (attempt %d)", reg_lsb, (uint8_t)(reg_lsb + 1), val, rcv, attempt);
        if (attempt == BQ256XX_WRITE_RETRIES)
        {
            e = ESP_ERR_INVALID_RESPONSE;
        }
#else
        break;
#endif
    }
    unlock((bq256xx_t *)dev);
    return e;
}

esp_err_t bq256xx_set_vbatreg_uV(const bq256xx_t *dev, int vbat_uV)
{
    int code = vbat_uV / (int)BQ2560X_VBATREG_STEP_uV;
    if (code < 350)
        code = 350;
    if (code > 480)
        code = 480;

    uint16_t v16 = 0;
    ESP_RETURN_ON_ERROR(bq256xx_read_reg_u16(dev, BQ256XX_CHARGE_VOLTAGE_LIMIT, &v16), TAG, "read VREG");

    v16 &= (uint16_t)~BQ256XX_VBATREG_MASK;
    v16 |= (uint16_t)((code << BQ256XX_VBATREG_SHIFT) & BQ256XX_VBATREG_MASK);

    return bq256xx_write_reg_u16(dev, BQ256XX_CHARGE_VOLTAGE_LIMIT, v16);
}

esp_err_t bq256xx_set_vindpm_uV(const bq256xx_t *dev, int vindpm_uV)
{
    int code = vindpm_uV / (int)BQ256XX_VINDPM_STEP_uV;
    if (code < 0x5F)
        code = 0x5F;
    if (code > 0x1A4)
        code = 0x1A4;

    return bq256xx_update_bits_u16(dev,
                                   BQ256XX_INPUT_VOLTAGE_LIMIT,
                                   BQ256XX_VINDPM_MASK,
                                   (uint16_t)((((code) & 0x07) << 5) | (((code) >> 3) << 8)));
}

esp_err_t bq256xx_set_ichg_uA(const bq256xx_t *dev, int ichg_uA)
{
    int code = ichg_uA / BQ256XX_ICHG_STEP_uA;
    if (code < 0)
        code = 0;
    if (code > 0x3F)
        code = 0x3F;
    uint16_t v16 = 0;
    esp_err_t e = bq256xx_read_reg_u16(dev, BQ256XX_CHARGE_CURRENT_LIMIT, &v16);
    if (e != ESP_OK)
        return e;
    v16 &= (uint16_t)~((uint16_t)0x3F << 5);
    v16 |= (uint16_t)((code & 0x3F) << 5);
    return bq256xx_write_reg_u16(dev, BQ256XX_CHARGE_CURRENT_LIMIT, v16);
}

esp_err_t bq256xx_set_iterm_uA(const bq256xx_t *dev, int iterm_uA)
{
    int code = (iterm_uA - BQ256XX_ITERM_OFFSET_uA) / BQ256XX_ITERM_STEP_uA;
    if (code < 0)
        code = 0;
    if (code > 0x3F)
        code = 0x3F;
    return bq256xx_update_bits_u16(dev,
                                   BQ256XX_TERMINATION_CURRENT_LIMIT,
                                   BQ256XX_ITERM_MASK,
                                   (uint16_t)(code << BQ256XX_ITERM_SHIFT));
}

esp_err_t bq256xx_set_iinlim_uA(const bq256xx_t *dev, int iin_uA)
{
    int code = (iin_uA - (int)BQ256XX_IINDPM_OFFSET_uA) / (int)BQ256XX_IINDPM_STEP_uA;
    if (code < 0)
        code = 0;
    if (code > 0xFF)
        code = 0xFF;

    uint16_t v16 = 0;
    esp_err_t e = bq256xx_read_reg_u16(dev, BQ256XX_INPUT_CURRENT_LIMIT, &v16);
    if (e != ESP_OK)
        return e;

    uint8_t reg06 = (uint8_t)(v16 & 0xFF);
    uint8_t reg07 = (uint8_t)(v16 >> 8);

    reg06 = (uint8_t)((reg06 & 0x0F) | ((code & 0x0F) << 4));
    reg07 = (uint8_t)((reg07 & 0xF0) | ((code >> 4) & 0x0F));

    v16 = (uint16_t)reg06 | ((uint16_t)reg07 << 8);
    return bq256xx_write_reg_u16(dev, BQ256XX_INPUT_CURRENT_LIMIT, v16);
}

esp_err_t bq256xx_set_iprechg_uA(const bq256xx_t *dev, int iprech_uA)
{
    int code = (iprech_uA - (int)BQ256XX_IPRECHG_OFFSET_uA) / (int)BQ256XX_IPRECHG_STEP_uA;
    if (code < 1)
        code = 1;
    if (code > 0x1F)
        code = 0x1F;
    return bq256xx_update_bits_u16(dev,
                                   BQ256XX_PRE_CHG_CONTROL,
                                   BQ256XX_IPRECHG_MASK,
                                   (uint16_t)(code << BQ256XX_IPRECHG_SHIFT));
}

esp_err_t bq256xx_enable_ilim_pin(const bq256xx_t *dev, bool en)
{
    return bq256xx_update_bits(dev,
                               BQ256XX_CHARGER_CONTROL_3,
                               BQ256XX_C3_EN_EXTILIM,
                               en ? BQ256XX_C3_EN_EXTILIM : 0);
}

esp_err_t bq256xx_enable_adc(const bq256xx_t *dev, bool en)
{
    return bq256xx_update_bits(dev, BQ256XX_ADC_CONTROL, 0x80, en ? 0x80 : 0);
}

esp_err_t bq256xx_enable_ts_adc(const bq256xx_t *dev, bool en)
{
    const uint8_t mask = 0x04;
    const uint8_t set = en ? 0x00 : mask;
    return bq256xx_update_bits(dev, BQ256XX_ADC_FUNC_DISABLE_0, mask, set);
}

esp_err_t bq256xx_kick_watchdog(bq256xx_t *dev)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(bq256xx_read_reg(dev, BQ256XX_CHARGER_CONTROL_0, &v), TAG, "read CTRL0");
    v |= BQ256XX_C0_WD_RST;
    lock(dev);
    esp_err_t e = write_reg_verify_masked(dev, BQ256XX_CHARGER_CONTROL_0, v, (uint8_t)(reg_verify_mask(BQ256XX_CHARGER_CONTROL_0)));
    unlock(dev);
    return e;
}

esp_err_t bq256xx_check_presence(const bq256xx_t *dev, uint8_t *part_num)
{
    uint8_t pn = 0;
    esp_err_t e = bq256xx_read_reg(dev, BQ256XX_PART_INFORMATION, &pn);
    if (e == ESP_OK && part_num)
    {
        *part_num = (pn >> 3) & 0x7;
    }
    return e;
}

void bq256xx_notify_int(bq256xx_t *dev)
{
    if (dev)
        dev->int_flag = true;
}

esp_err_t bq256xx_set_charge_enable(const bq256xx_t *dev, bool en)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(bq256xx_read_reg(dev, BQ256XX_CHARGER_CONTROL_0, &v), TAG, "read C0");
    v = en ? (v | BQ256XX_C0_EN_CHG) : (v & ~BQ256XX_C0_EN_CHG);
    return bq256xx_write_reg(dev, BQ256XX_CHARGER_CONTROL_0, v);
}

esp_err_t bq256xx_set_hiz(const bq256xx_t *dev, bool en)
{
    uint8_t v = 0;
    ESP_RETURN_ON_ERROR(bq256xx_read_reg(dev, BQ256XX_CHARGER_CONTROL_0, &v), TAG, "read C0");
    v = en ? (v | BQ256XX_C0_EN_HIZ) : (v & ~BQ256XX_C0_EN_HIZ);
    return bq256xx_write_reg(dev, BQ256XX_CHARGER_CONTROL_0, v);
}

esp_err_t bq256xx_set_watchdog(const bq256xx_t *dev, bq256xx_wdt_t w)
{
    return bq256xx_update_bits(dev, BQ256XX_CHARGER_CONTROL_0, BQ256XX_C0_WDT_MASK, (uint8_t)w);
}

esp_err_t bq256xx_enter_ship_mode(const bq256xx_t *dev)
{
    ESP_RETURN_ON_ERROR(bq256xx_set_charge_enable(dev, false), TAG, "chg disable");
    ESP_RETURN_ON_ERROR(bq256xx_set_hiz(dev, true), TAG, "hiz on");
    return ESP_OK;
}

static inline uint8_t timer_fast_code_from_hours(int h)
{
    if (h <= 5)
        return 0;
    if (h <= 8)
        return 1;
    if (h <= 10)
        return 2;
    return 3;
}

static inline uint8_t timer_pre_code_from_minutes(int m)
{
    if (m <= 30)
        return 0;
    if (m <= 45)
        return 1;
    if (m <= 60)
        return 2;
    return 3;
}

esp_err_t bq256xx_set_safety_timers(const bq256xx_t *dev,
                                    bool enable, int fastcharge_hours, int precharge_minutes)
{
    uint8_t v = 0;

    v |= BQ256XX_TMR_TMR2X_EN;
    if (enable)
        v |= BQ256XX_TMR_EN_SAFETY;

    if (fastcharge_hours >= 21)
        v |= BQ256XX_TMR_CHG_TMR;

    if (precharge_minutes <= 60)
        v |= BQ256XX_TMR_PRECHG_TMR;

    return bq256xx_write_reg(dev, BQ256XX_CHARGER_TIMER_CONTROL, v);
}

esp_err_t bq256xx_set_ts_ignore(const bq256xx_t *dev, bool ignore)
{
    return bq256xx_update_bits(dev, BQ256XX_NTC_CONTROL_0, 0x80, ignore ? 0x80 : 0x00);
}

esp_err_t bq256xx_set_treg_120(const bq256xx_t *dev)
{
    return bq256xx_update_bits(dev, BQ256XX_CHARGER_CONTROL_1, BQ256XX_C1_TREG_MASK, BQ256XX_C1_TREG_120C);
}
