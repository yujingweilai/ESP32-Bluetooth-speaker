#include "board.h"
#include "app_bms.h"


#define INPUT_DEBOUNCE      (500)        //50s
#define OUTPUT_DEBOUNCE     (200)        //20s
#define BAT_VOL_02P         (3350)       // %10, 3.5V
#define BAT_VOL_25P         (3750)
#define BAT_VOL_50P         (3850)
#define BAT_VOL_75P         (4000)
#define BAT_VOL_100P        (4200)
#define ABS(a)          (a > 0 ? a : -a)

void battery_handle_100ms(void);

//Global variable
static uint8_t bat_cap_expect;
static bit_t bat_had_low;
static uint8_t bat_low_cnt;
static uint8_t battery_batcap;
static uint16_t bat_display_cap;
static uint8_t bat_less_than_10_cnt;

const uint16_t bat_cap_cmp_val[6] = {
    /* 0%, 10%, 25%, 50%, 75%, 100% */
    CONFIG_BAT_LOW_VOLTAGE,
    BAT_VOL_02P,
    BAT_VOL_25P,
    BAT_VOL_50P,
    BAT_VOL_75P,
    BAT_VOL_100P,
};

const uint8_t bat_cap_step_val[6] = {
    0, 1, 2, 25, 50, 75,
};

const uint8_t bat_cap_percent_val[5] = {
    1, 23, 25, 25, 25
};

bool batcap_is_init;

void battery_init(void)
{
    /* Update batcap when system is waked up */
    batcap_is_init = false;
    Task_CreatTask(TASKID_BATTERY,battery_handle_100ms,50);
    Task_WakeUp(TASKID_BATTERY);

}

static uint16_t cal_bat_cap(uint8_t bat_cap_step)
{
    uint16_t bat_current_cap;
    int16_t vbat;
    
    vbat = adc_get_vbat();
    
    if (bat_cap_step == 6) {
        bat_current_cap = 100;
    } else if (bat_cap_step == 0) {
        bat_current_cap = 0;
    } else {
        bat_current_cap = (vbat - bat_cap_cmp_val[bat_cap_step - 1]) * bat_cap_percent_val[bat_cap_step - 1];
        bat_current_cap /= (bat_cap_cmp_val[bat_cap_step] - bat_cap_cmp_val[bat_cap_step - 1]);
        bat_current_cap += bat_cap_step_val[bat_cap_step];
    } 
    return bat_current_cap;
}

static void battery_handle_cap(void)
{
    uint8_t read_cnt;
    static uint16_t delay_cnt;
    uint8_t idx;
    int16_t vbat;
    uint16_t bat_current_cap;
    
    read_cnt = 0 ;
    vbat = adc_get_vbat();
   
    for (idx = 0; idx < 6; idx++) {
        if (vbat < bat_cap_cmp_val[idx])
            break;
    }
    bat_current_cap = cal_bat_cap(idx);
    // debug_print("bat_current_cap:%d ",bat_current_cap);
    if (batcap_is_init) {
        /*
         *If power is dcin OK, batcap can't become higher than before.
         *If power is dcin not OK, batcap can't become lower than before.
         */
		delay_cnt++;
        if (BAT_IS_CHARGING()) {
            if (bat_current_cap <= bat_display_cap) {
                delay_cnt = 0;
            } else {
                /*batcap plus 1 percent every one minute till bat_display_cap == bat_current_cap*/
                if (delay_cnt == INPUT_DEBOUNCE) {
                    delay_cnt = 0;
                    bat_display_cap++;
                } 
            }
        } else {
            if (bat_current_cap >= bat_display_cap) {
                delay_cnt = 0;
            } else {
                /*batcap sub 1 percent every 10s till bat_display_cap == bat_current_cap*/
                if (delay_cnt == OUTPUT_DEBOUNCE) {
                    delay_cnt = 0;
                    bat_display_cap--;
                } 
            }
        }
    }
    
    /*save current batcap*/
    if (batcap_is_init == false) {
        bat_display_cap = bat_current_cap;
        batcap_is_init = true;         
        Task_CreatTask(TASKID_BATTERY,battery_handle_100ms,100);
    }

    if (bat_is_low_voltage()) {
        bat_display_cap = 0;
    }
    // debug_print("bat_display_cap:%d\n",bat_display_cap);
}

void battery_handle_low(void)
{
    int16_t vbat;

    vbat = adc_get_vbat();

	bat_low_cnt++;
    if (bat_had_low) {
        if (BAT_IS_CHARGING() && (vbat >= CONFIG_BAT_LOW_VOLTAGE + CONFIG_BAT_LOW_DEBOUNCE))
            bat_low_cnt = 0;
    } else {
        if (vbat >= CONFIG_BAT_LOW_VOLTAGE)
            bat_low_cnt = 0;
    }
    if (bat_low_cnt >= CONFIG_BAT_LOW_CNT) {
        bat_had_low = true;
        bat_low_cnt = CONFIG_BAT_LOW_CNT;
    } else {
        bat_had_low = false;
    }
	
}

void battery_handle_100ms(void)
{
    battery_handle_low();
    battery_handle_cap();
}

bool bat_is_low_voltage(void)
{
    /* if go here, sys state is not starting */
    return bat_had_low;
}

uint8_t bat_get_cap(void)
{
    return bat_display_cap;
}

