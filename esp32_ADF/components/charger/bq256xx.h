#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifndef BQ256XX_I2C_ADDR
#define BQ256XX_I2C_ADDR 0x6A
#endif
#define BQ256XX_ADDR BQ256XX_I2C_ADDR


#define BQ256XX_CHARGE_CURRENT_LIMIT 0x02
#define BQ256XX_CHARGE_VOLTAGE_LIMIT 0x04
#define BQ256XX_INPUT_CURRENT_LIMIT 0x06
#define BQ256XX_MINIMAL_VSYS 0x0E
#define BQ256XX_INPUT_VOLTAGE_LIMIT 0x08
#define BQ256XX_PRE_CHG_CONTROL 0x10
#define BQ256XX_TERMINATION_CONTROL 0x12
#define BQ256XX_TERMINATION_CURRENT_LIMIT BQ256XX_TERMINATION_CONTROL
#define BQ256XX_CHARGE_CONTROL 0x14
#define BQ256XX_CHARGER_TIMER_CONTROL 0x15
#define BQ256XX_TMR_DIS_STAT               (1u << 7)
#define BQ256XX_TMR_TMR2X_EN               (1u << 3)
#define BQ256XX_TMR_EN_SAFETY              (1u << 2)
#define BQ256XX_TMR_PRECHG_TMR             (1u << 1)
#define BQ256XX_TMR_CHG_TMR                (1u << 0)
#define BQ256XX_CHARGER_CONTROL_0 0x16
#define BQ256XX_CHARGER_CONTROL_1 0x17
#define BQ256XX_C1_TREG_SHIFT               0
#define BQ256XX_C1_TREG_MASK                (0x03u << BQ256XX_C1_TREG_SHIFT)
#define BQ256XX_C1_TREG_60C                 (0x00u << BQ256XX_C1_TREG_SHIFT)
#define BQ256XX_C1_TREG_80C                 (0x01u << BQ256XX_C1_TREG_SHIFT)
#define BQ256XX_C1_TREG_100C                (0x02u << BQ256XX_C1_TREG_SHIFT)
#define BQ256XX_C1_TREG_120C                (0x03u << BQ256XX_C1_TREG_SHIFT)
#define BQ256XX_CHARGER_CONTROL_2 0x18
#define BQ256XX_CHARGER_CONTROL_3 0x19
#define BQ256XX_NTC_CONTROL_0 0x1A
#define BQ256XX_NTC_CONTROL_1 0x1B
#define BQ256XX_NTC_CONTROL_2 0x1C

/*******************************************************************************
 * NTC Control Register Bit Definitions (NTC 控制寄存器位定义)
 * 
 * Reference: BQ25628E Datasheet, Section 9.6 NTC Control Registers
 ******************************************************************************/

/*------------------------------------------------------------------------------
 * REG0x1A - NTC_CONTROL_0 (Default: 0x0D)
 *
 * Bit 7:   TS_IGNORE    - 温度传感器忽略位
 *                         0 = NTC 监测启用 (默认, 推荐)
 *                         1 = 忽略 NTC, 芯片不执行温度保护 (危险!)
 *
 * Bits 6:4 Reserved (BQ25628E defines these as reserved; keep 0)

 * Bits 3:2 TS_ISET_WARM - Warm 区间充电电流设置 (45°C ~ 60°C)
 *                         00 = 充电暂停 (Charge Suspend)
 *                         01 = 20% ICHG
 *                         10 = 40% ICHG
 *                         11 = 100% ICHG (默认, 不降低)
 *
 * Bits 1:0 TS_ISET_COOL - Cool 区间充电电流设置 (0°C ~ 10°C)
 *                         00 = 充电暂停 (Charge Suspend)
 *                         01 = 20% ICHG (默认)
 *                         10 = 40% ICHG
 *                         11 = 100% ICHG (不降低)
 *----------------------------------------------------------------------------*/
#define BQ256XX_NTC0_TS_IGNORE          (1u << 7)   /* 忽略 NTC 温度传感器 */
#define BQ256XX_NTC0_TS_OPEN            (1u << 5)   /* Reserved on BQ25628E; do not set */
#define BQ256XX_NTC0_TSHUT_TH           (1u << 4)   /* Reserved on BQ25628E; do not set */

/* TS_ISET_WARM (Bits 3:2) - Warm 区间电流设置 */
#define BQ256XX_NTC0_TS_ISET_WARM_SHIFT     2
#define BQ256XX_NTC0_TS_ISET_WARM_MASK      (0x03u << BQ256XX_NTC0_TS_ISET_WARM_SHIFT)
#define BQ256XX_NTC0_TS_ISET_WARM_SUSPEND   (0x00u << BQ256XX_NTC0_TS_ISET_WARM_SHIFT)  /* 00b: 暂停充电 */
#define BQ256XX_NTC0_TS_ISET_WARM_20PCT     (0x01u << BQ256XX_NTC0_TS_ISET_WARM_SHIFT)  /* 01b: 20% 电流 */
#define BQ256XX_NTC0_TS_ISET_WARM_40PCT     (0x02u << BQ256XX_NTC0_TS_ISET_WARM_SHIFT)  /* 10b: 40% 电流 */
#define BQ256XX_NTC0_TS_ISET_WARM_100PCT    (0x03u << BQ256XX_NTC0_TS_ISET_WARM_SHIFT)  /* 11b: 100% 电流 (默认) */

/* TS_ISET_COOL (Bits 1:0) - Cool 区间电流设置 */
#define BQ256XX_NTC0_TS_ISET_COOL_SHIFT     0
#define BQ256XX_NTC0_TS_ISET_COOL_MASK      (0x03u << BQ256XX_NTC0_TS_ISET_COOL_SHIFT)
#define BQ256XX_NTC0_TS_ISET_COOL_SUSPEND   (0x00u << BQ256XX_NTC0_TS_ISET_COOL_SHIFT)  /* 00b: 暂停充电 */
#define BQ256XX_NTC0_TS_ISET_COOL_20PCT     (0x01u << BQ256XX_NTC0_TS_ISET_COOL_SHIFT)  /* 01b: 20% 电流 (默认) */
#define BQ256XX_NTC0_TS_ISET_COOL_40PCT     (0x02u << BQ256XX_NTC0_TS_ISET_COOL_SHIFT)  /* 10b: 40% 电流 */
#define BQ256XX_NTC0_TS_ISET_COOL_100PCT    (0x03u << BQ256XX_NTC0_TS_ISET_COOL_SHIFT)  /* 11b: 100% 电流 */

/*------------------------------------------------------------------------------
 * REG0x1B - NTC_CONTROL_1 (Default: 0x25)
 *
 * Bits 7:5 TS_TH1_TH2_TH3 - Cold/Cool thresholds (103AT, RT1=5.24k, RT2=30.31k)
 *                          000 = TH1=0°C,  TH2=5°C,  TH3=15°C
 *                          001 = TH1=0°C,  TH2=10°C, TH3=15°C (default)
 *                          010 = TH1=0°C,  TH2=15°C, TH3=20°C
 *                          011 = TH1=0°C,  TH2=20°C, TH3=20°C
 *                          100 = TH1=-5°C, TH2=5°C,  TH3=15°C
 *                          101 = TH1=-5°C, TH2=10°C, TH3=15°C
 *                          110 = TH1=-5°C, TH2=10°C, TH3=20°C
 *                          111 = TH1=0°C,  TH2=10°C, TH3=20°C

 * Bits 4:2 TS_TH4_TH5_TH6 - Warm/Hot thresholds (103AT, RT1=5.24k, RT2=30.31k)
 *                          000 = TH4=35°C, TH5=40°C, TH6=60°C
 *                          001 = TH4=35°C, TH5=45°C, TH6=60°C (default)
 *                          010 = TH4=35°C, TH5=50°C, TH6=60°C
 *                          011 = TH4=40°C, TH5=55°C, TH6=60°C
 *                          100 = TH4=35°C, TH5=40°C, TH6=50°C
 *                          101 = TH4=35°C, TH5=45°C, TH6=50°C
 *                          110 = TH4=40°C, TH5=45°C, TH6=60°C
 *                          111 = TH4=40°C, TH5=50°C, TH6=60°C

 * Bits 1:0 TS_VSET_WARM - Warm 区间电压设置
 *                         00 = VBATREG - 300mV
 *                         01 = VBATREG - 200mV (默认)
 *                         10 = VBATREG - 100mV
 *                         11 = VBATREG (不降低)
 *----------------------------------------------------------------------------*/
#define BQ256XX_NTC1_TS_TH123_SHIFT         5
#define BQ256XX_NTC1_TS_TH123_MASK          (0x07u << BQ256XX_NTC1_TS_TH123_SHIFT)
#define BQ256XX_NTC1_TS_TH123_DEFAULT       (0x01u << BQ256XX_NTC1_TS_TH123_SHIFT)  /* 001b: 0/10/15°C (默认) */

#define BQ256XX_NTC1_TS_TH456_SHIFT         2
#define BQ256XX_NTC1_TS_TH456_MASK          (0x07u << BQ256XX_NTC1_TS_TH456_SHIFT)
#define BQ256XX_NTC1_TS_TH456_DEFAULT       (0x01u << BQ256XX_NTC1_TS_TH456_SHIFT)  /* 001b: 35/45/60°C (默认) */

#define BQ256XX_NTC1_TS_VSET_WARM_SHIFT     0
#define BQ256XX_NTC1_TS_VSET_WARM_MASK      (0x03u << BQ256XX_NTC1_TS_VSET_WARM_SHIFT)
#define BQ256XX_NTC1_TS_VSET_WARM_M300MV    (0x00u << BQ256XX_NTC1_TS_VSET_WARM_SHIFT)  /* 00b: -300mV */
#define BQ256XX_NTC1_TS_VSET_WARM_M200MV    (0x01u << BQ256XX_NTC1_TS_VSET_WARM_SHIFT)  /* 01b: -200mV (默认) */
#define BQ256XX_NTC1_TS_VSET_WARM_M100MV    (0x02u << BQ256XX_NTC1_TS_VSET_WARM_SHIFT)  /* 10b: -100mV */
#define BQ256XX_NTC1_TS_VSET_WARM_NONE      (0x03u << BQ256XX_NTC1_TS_VSET_WARM_SHIFT)  /* 11b: 不降低 */

/*------------------------------------------------------------------------------
 * REG0x1C - NTC_CONTROL_2 (Default: 0x3F)
 * 其他高级阈值配置，通常保持默认值
 *----------------------------------------------------------------------------*/
#define BQ256XX_NTC2_DEFAULT                0x3F

/*******************************************************************************
 * JEITA NTC Configuration Presets (JEITA NTC 配置预设)
 * 
 * 硬件电路: R_T1 = 5.23kΩ (上拉), R_T2 = 30.1kΩ (并联), 103AT NTC
 * 
 * 配置说明:
 * - NTC 监测: 启用 (TS_IGNORE = 0)
 * - Cool 区间 (0-10°C): 电流降至 20%
 * - Warm 区间 (45-60°C): 充电暂停 (电流 = 0)  <-- 用户要求
 * - 温度阈值: 使用推荐默认值 (0°C ~ 60°C 保护范围)
 * - Warm 区间电压: 降低 200mV
 ******************************************************************************/

/*
 * REG0x1A 配置值计算:
 * Bit 7 (TS_IGNORE)   = 0  (启用 NTC)
 * Bit 6 (Reserved)    = 0
 * Bit 5 (Reserved)    = 0
 * Bit 4 (Reserved)    = 0
 * Bit 3:2 (TS_ISET_WARM) = 00 (Warm 区间暂停充电) <-- 用户要求
 * Bit 1:0 (TS_ISET_COOL) = 01 (Cool 区间 20% 电流, 默认)
 * 
 * 计算: 0b_0000_0001 = 0x01
 */
#define BQ256XX_NTC0_CFG_WARM_SUSPEND   ( \
    0u                                  | /* TS_IGNORE = 0: 启用 NTC 监测 */ \
    0u                                  | /* TS_OPEN = 0: TS 开路禁止充电 */ \
    0u                                  | /* TSHUT_TH = 0: 热关断 130°C */ \
    BQ256XX_NTC0_TS_ISET_WARM_SUSPEND   | /* Warm 区间: 暂停充电 (00b) */ \
    BQ256XX_NTC0_TS_ISET_COOL_20PCT       /* Cool 区间: 20% 电流 (01b, 默认) */ \
)   /* = 0x01 */

/*
 * REG0x1B 配置值 (保持默认):
 * Bit 7:5 (TS_TH123) = 001 (0/10/15°C 阈值)
 * Bit 4:2 (TS_TH456) = 001 (35/45/60°C 阈值)
 * Bit 1:0 (TS_VSET_WARM) = 01 (Warm 电压降低 200mV)
 * 
 * 计算: 0b_0010_0101 = 0x25
 */
#define BQ256XX_NTC1_CFG_DEFAULT    ( \
    BQ256XX_NTC1_TS_TH123_DEFAULT   | /* 低温阈值: 0/10/15°C (默认) */ \
    BQ256XX_NTC1_TS_TH456_DEFAULT   | /* 高温阈值: 35/45/60°C (默认) */ \
    BQ256XX_NTC1_TS_VSET_WARM_M200MV  /* Warm 电压: 降低 200mV (默认) */ \
)   /* = 0x25 */

/*
 * REG0x1C 配置值 (保持默认):
 */
#define BQ256XX_NTC2_CFG_DEFAULT        BQ256XX_NTC2_DEFAULT   /* = 0x3F */

/*------------------------------------------------------------------------------
 * REG0x1F - FAULT_STATUS_0 TS_STAT 字段定义
 * 
 * Bits 2:0 TS_STAT - 温度传感器状态
 *----------------------------------------------------------------------------*/
#define BQ256XX_TS_STAT_MASK            0x07
#define BQ256XX_TS_STAT_NORMAL          0x00    /* 000: TS_NORMAL */
#define BQ256XX_TS_STAT_COLD            0x01    /* 001: TS_COLD or TS resistor not available */
#define BQ256XX_TS_STAT_HOT             0x02    /* 010: TS_HOT */
#define BQ256XX_TS_STAT_COOL            0x03    /* 011: TS_COOL */
#define BQ256XX_TS_STAT_WARM            0x04    /* 100: TS_WARM */
#define BQ256XX_TS_STAT_PRECOOL         0x05    /* 101: TS_PRECOOL */
#define BQ256XX_TS_STAT_PREWARM         0x06    /* 110: TS_PREWARM */
#define BQ256XX_TS_STAT_FAULT           0x07    /* 111: TS pin bias fault */
#define BQ256XX_CHARGER_STATUS_0 0x1D
#define BQ256XX_CHARGER_STATUS_1 0x1E
#define BQ256XX_FAULT_STATUS 0x1F
#define BQ256XX_CHARGER_FLAG_0 0x20
#define BQ256XX_CHARGER_FLAG_1 0x21
#define BQ256XX_FAULT_0 0x22
#define BQ256XX_CHARGER_MASK_0 0x23
#define BQ256XX_CHARGER_MASK_1 0x24
#define BQ256XX_FAULT_MASK 0x25
#define BQ256XX_ADC_CONTROL 0x26
#define BQ256XX_ADC_FUNC_DISABLE_0 0x27
#define BQ256XX_IBUS_ADC 0x28
#define BQ256XX_IBAT_ADC 0x2A
#define BQ256XX_VBUS_ADC 0x2C
#define BQ256XX_VPMID_ADC 0x2E
#define BQ256XX_VBAT_ADC 0x30
#define BQ256XX_VSYS_ADC 0x32
#define BQ256XX_TS_ADC 0x34
#define BQ256XX_TDIE_ADC 0x36
#define BQ256XX_PART_INFORMATION 0x38
#define BQ256XX_C0_EN_CHG (1u << 5)
#define BQ256XX_C0_EN_HIZ (1u << 4)
#define BQ256XX_C0_WD_RST (1u << 2)
#define BQ256XX_C0_WDT_MASK (0x3u << 0)
#define BQ256XX_ICHG_MASK 0x3F
#define BQ256XX_ICHG_STEP_uA 40000
#define BQ256XX_ICHG_OFFSET_uA 60000
#define BQ256XX_ICHG_SHIFT 0
#define BQ256XX_ITERM_MASK 0xFC
#define BQ256XX_ITERM_STEP_uA 5000
#define BQ256XX_ITERM_OFFSET_uA 5000
#define BQ256XX_ITERM_SHIFT 2
#define BQ256XX_IPRECHG_MASK 0xF8
#define BQ256XX_IPRECHG_STEP_uA 10000
#define BQ256XX_IPRECHG_OFFSET_uA 0
#define BQ256XX_IPRECHG_SHIFT 3
#define BQ256XX_VBATREG_MASK 0x0FF8
#define BQ2560X_VBATREG_STEP_uV 10000
#define BQ2560X_VBATREG_OFFSET_uV 0
#define BQ256XX_VBATREG_SHIFT 3
#define BQ256XX_VINDPM_STEP_uV 40000
#define BQ256XX_VINDPM_OFFSET_uV 0
#define BQ256XX_IINDPM_MASK 0x1F
#define BQ256XX_IINDPM_STEP_uA 20000
#define BQ256XX_IINDPM_OFFSET_uA 100000
#define BQ256XX_ADC_DONE_STAT_MASK (1u << 6)
#define BQ256XX_TREG_STAT_MASK (1u << 5)
#define BQ256XX_VSYS_STAT_MASK (1u << 4)
#define BQ256XX_IINDPM_STAT_MASK (1u << 3)
#define BQ256XX_VINDPM_STAT_MASK (1u << 2)
#define BQ256XX_SAFETY_TMR_STAT_MASK (1u << 1)
#define BQ256XX_WD_STAT_MASK (1u << 0)
#define BQ256XX_VBUS_STAT_MASK 0xE0
#define BQ256XX_CHRG_STAT_MASK 0x18
#define BQ256XX_C3_EN_EXTILIM (1u << 2)
#define BQ256XX_VINDPM_MASK 0x3FE0

typedef struct
{
    bool bq_present;
    bool is_charging;
    float vbus_voltage;
    float vbat_voltage;
    float ibatt_current;
    float iin_current;
    float vbus_current;
    float vbat_current;
    float vbat_temp;
    float vsys_voltage;
} bq256xx_msg_t;

typedef struct
{
    uint8_t vbus_stat;
    uint8_t chrg_stat;
    bool power_good;
} bq256xx_basic_stat_t;
