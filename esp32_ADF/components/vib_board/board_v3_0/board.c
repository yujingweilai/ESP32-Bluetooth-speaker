/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2019 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
 *
 * Permission is hereby granted for use on all ESPRESSIF SYSTEMS products, in which case,
 * it is free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the Software is furnished
 * to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "esp_log.h"
#include "board.h"
#include "audio_mem.h"
#include "board_def.h"
#include "board_pins_config_custom.h"
#include "periph_sdcard.h"
#include "led_indicator.h"
#include "periph_key.h"
#include "driver/adc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "periph_battery.h"
#include "esp_task.h"
#include "charger.h"
#include "esp_check.h"
#include "../../../main/lib/fw_timer.h"
#include "../../../main/app_player.h"
#include "oled_ssd1315.h"

static const char *TAG = "VIB_AUDIO_BOARD";

/* I2C master bus and device handles */
static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_i2c_dev_charger = NULL;
static i2c_master_dev_handle_t s_i2c_dev_oled = NULL;

static audio_board_handle_t board_handle = 0;
static esp_periph_handle_t sdcard_handle = 0;
static esp_periph_handle_t battery_handle = NULL;
static esp_periph_handle_t key_periph = NULL;
static oled_t *s_oled = NULL;
static int s_oled_track_num = 0;
#define OLED_I2C_ADDR (0x3C)

audio_board_handle_t audio_board_init(void)
{
    ESP_LOGI(TAG, "[*]vib board v1 audio_board_init");
    if (board_handle)
    {
        ESP_LOGW(TAG, "The board has already been initialized!");
        return board_handle;
    }
    FW_TimerInit();
    // config LTK5302
    //  gpio_config_t io_conf = {};
    //  io_conf.intr_type = GPIO_INTR_DISABLE;
    //  io_conf.mode = GPIO_MODE_OUTPUT;
    //  io_conf.pin_bit_mask = (1ULL<<GPIO_AMP_EN);
    //  io_conf.pull_down_en = 0;
    //  io_conf.pull_up_en = 0;
    //  gpio_config(&io_conf);
    //  esp_rom_delay_us(10);
    //  gpio_set_level(GPIO_AMP_EN,1);
    //  gpio_set_level(GPIO_AMP_EN,1);
    //  esp_rom_delay_us(10);
    //  gpio_set_level(GPIO_AMP_EN,0);
    //  esp_rom_delay_us(10);
    //  gpio_set_level(GPIO_AMP_EN,1);

    board_handle = (audio_board_handle_t)audio_calloc(1, sizeof(struct audio_board_handle));
    AUDIO_MEM_CHECK(TAG, board_handle, return NULL);
    board_handle->audio_hal = audio_board_codec_init();
    if (board_handle->audio_hal == NULL)
    {
        audio_free(board_handle);
        ESP_LOGE(TAG, "audio_board_codec_init failed");
        return NULL;
    }

    return board_handle;
}

extern esp_err_t es8388_set_voice_volume(int volume);
extern esp_err_t es8311_codec_set_voice_volume(int volume);

static volatile int s_user_vol_target = -1;
static volatile int s_user_vol_displayed = -1;
static volatile int s_runtime_vol_limit = -1;
static volatile int s_effective_vol_applied = -1;
static volatile bool s_timer_pending = false;
static volatile bool s_volume_ui_ready = false;

static int audio_board_clip_volume(int volume)
{
    if (volume < 0) {
        return 0;
    }
    if (volume > 100) {
        return 100;
    }
    return volume;
}

static int audio_board_get_effective_volume(void)
{
    int user_vol = s_user_vol_target;
    int runtime_limit = s_runtime_vol_limit;

    if (user_vol < 0) {
        user_vol = 0;
    }
    if (runtime_limit < 0) {
        return audio_board_clip_volume(user_vol);
    }
    if (runtime_limit < user_vol) {
        user_vol = runtime_limit;
    }
    return audio_board_clip_volume(user_vol);
}

static void audio_board_vol_timer_cb(U16 timerId, void *arg)
{
    int effective_vol;
    int user_vol;

    (void)timerId;
    (void)arg;
    s_timer_pending = false;

    effective_vol = audio_board_get_effective_volume();
    if (effective_vol != s_effective_vol_applied)
    {
#ifdef CONFIG_AUDIO_CODEC_ES8388
        es8388_set_voice_volume(effective_vol);
#endif
#ifdef CONFIG_AUDIO_CODEC_ES8311
        esp_err_t ret = es8311_codec_set_voice_volume(effective_vol);
        (void)ret;
        ESP_LOGI(TAG, "es8311_codec_set_voice_volume %d", effective_vol);
#endif
        s_effective_vol_applied = effective_vol;
    }

    user_vol = audio_board_clip_volume(s_user_vol_target);
    if (s_volume_ui_ready && user_vol != s_user_vol_displayed)
    {
        displayer_volume(user_vol);
        s_user_vol_displayed = user_vol;
    }

    /* 如果运行期限幅或用户音量在下发期间又发生变化，则继续补一次。 */
    if (audio_board_get_effective_volume() != s_effective_vol_applied ||
        (s_volume_ui_ready &&
         audio_board_clip_volume(s_user_vol_target) != s_user_vol_displayed)) {
        s_timer_pending = true;
        FW_SetTimer(audio_board_vol_timer_cb, 1, NULL, 50);
    }
}

esp_err_t audio_board_set_voice_volume(int volume)
{
    s_user_vol_target = audio_board_clip_volume(volume);
    if (!s_timer_pending)
    {
        s_timer_pending = true;
        FW_SetTimer(audio_board_vol_timer_cb, 1, NULL, 50);
    }
    return ESP_OK;
}

void audio_board_enable_volume_ui(void)
{
    s_volume_ui_ready = true;
    s_user_vol_displayed = -1;
    if (!s_timer_pending)
    {
        s_timer_pending = true;
        FW_SetTimer(audio_board_vol_timer_cb, 1, NULL, 1);
    }
}

esp_err_t audio_board_get_user_voice_volume(int *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_user_vol_target < 0) {
        *volume = 0;
    } else {
        *volume = audio_board_clip_volume(s_user_vol_target);
    }
    return ESP_OK;
}

esp_err_t audio_board_set_runtime_voice_limit(int volume)
{
    if (volume < 0) {
        s_runtime_vol_limit = -1;
    } else {
        s_runtime_vol_limit = audio_board_clip_volume(volume);
    }

    if (!s_timer_pending)
    {
        s_timer_pending = true;
        FW_SetTimer(audio_board_vol_timer_cb, 1, NULL, 50);
    }

    return ESP_OK;
}



audio_hal_handle_t audio_board_codec_init(void)
{
    audio_hal_codec_config_t audio_codec_cfg = AUDIO_CODEC_DEFAULT_CONFIG();
#ifdef CONFIG_AUDIO_CODEC_ES8388
    AUDIO_CODEC_ES8388_DEFAULT_HANDLE.audio_codec_set_volume = audio_board_set_voice_volume;
    audio_hal_handle_t codec_hal = audio_hal_init(&audio_codec_cfg, &AUDIO_CODEC_ES8388_DEFAULT_HANDLE);
    ESP_LOGW(TAG, "audio_board_codec_init: ES8388");
#endif
#ifdef CONFIG_AUDIO_CODEC_ES8311
    AUDIO_CODEC_ES8311_DEFAULT_HANDLE.audio_codec_set_volume = audio_board_set_voice_volume;
    audio_hal_handle_t codec_hal = audio_hal_init(&audio_codec_cfg, &AUDIO_CODEC_ES8311_DEFAULT_HANDLE);
    ESP_LOGW(TAG, "audio_board_codec_init: ES8311");
#endif
    AUDIO_NULL_CHECK(TAG, codec_hal, return NULL);
    return codec_hal;
}

display_service_handle_t audio_board_led_init(void)
{
    led_indicator_handle_t led = led_indicator_init((gpio_num_t)get_green_led_gpio());
    display_service_config_t display = {
        .based_cfg = {
            .task_stack = 0,
            .task_prio = 0,
            .task_core = 0,
            .task_func = NULL,
            .service_start = NULL,
            .service_stop = NULL,
            .service_destroy = NULL,
            .service_ioctl = led_indicator_pattern,
            .service_name = "DISPLAY_serv",
            .user_data = NULL,
        },
        .instance = led,
    };

    return display_service_create(&display);
}

bool is_comb_button(uint64_t mask)
{
    // 根据 id 二进制有多个 1 的为组合按键
    int count = 0;
    while (mask > 0)
    {
        mask &= (mask - 1);
        count++;
    }
    return count > 1;
}
typedef struct
{
    int key_id;
    int gpio_num;
} button_map_t;

static const button_map_t button_map[] = {
    {KEY_ID_PREV, 36},
    {KEY_ID_NEXT, 37},
    {KEY_ID_PLAY, 38},
};

#define BTN_HARD_NUM (sizeof(button_map) / sizeof(button_map[0]))

static int board_get_key_gpio(int key_id)
{
    for (size_t i = 0; i < BTN_HARD_NUM; i++)
    {
        if (button_map[i].key_id == key_id)
        {
            return button_map[i].gpio_num;
        }
    }

    return -1;
}

static bool board_is_key_pressed(int key_id)
{
    int gpio_num = board_get_key_gpio(key_id);

    if (gpio_num < 0)
    {
        return false;
    }

    return gpio_get_level(gpio_num) == 0;
}

#define TOUCH_LOCK_GUARD_TICKS pdMS_TO_TICKS(80)
#define TOUCH_LOCK_COMBO_CONFIRM_CNT 3
#define PLAY_BOOT_RELEASE_STABLE_MS 500
#define OLED_AUTO_SLEEP_TIMEOUT_MS (3 * 60 * 1000) //3分钟屏幕自动息屏宏定义
#define TIMER_ID_OLED_AUTO_SLEEP 0x4F01U

typedef enum
{
    PLAY_BOOT_GUARD_READY = 0,
    PLAY_BOOT_GUARD_WAIT_RELEASE,
    PLAY_BOOT_GUARD_CONFIRM_RELEASE,
} play_boot_guard_state_t;

/* PLAY/NEXT 组合键锁存标志，组合一旦建立后保持虚拟键输出，避免抖动回落为单键 */
static bool s_touch_lock_combo_latched = false;
/* PLAY+NEXT 组合抗干扰计数，连续命中达到阈值后才认定进入组合键 */
static uint8_t s_touch_lock_combo_confirm_cnt = 0;
/* PLAY/NEXT 单键候选键值，在短观察窗内先不上报，给组合键留出并入时间 */
static int s_touch_lock_candidate_key = -1;
/* PLAY/NEXT 单键候选起始 tick，用于判断是否结束组合观察窗 */
static TickType_t s_touch_lock_candidate_tick = 0;
/* 观察窗内提前松手时，补发一次单键按下，下一拍自动生成短按释放事件 */
static int s_touch_lock_release_inject_key = -1;
/* 按键开机后，PLAY 连续稳定松开前不再向上层透传，避免残留触摸触发关机 */
static play_boot_guard_state_t s_play_boot_guard_state = PLAY_BOOT_GUARD_READY;
static TickType_t s_play_boot_release_tick = 0;
/* OLED 自动息屏总开关，仅在真正进入系统后由上层显式开启 */
static bool s_oled_auto_sleep_enabled = false;
/* OLED 当前是否处于 0xAE 关屏状态 */
static bool s_oled_is_sleeping = false;
/* 唤醒后的吞键锁存，确保第一次触摸仅唤醒不透传功能 */
static bool s_oled_wake_swallow_active = false;

/**
 * @brief 函数名称: board_reset_touch_key_state
 * @param 无
 * @return 无
 * @note 作用: 清理底层按键组合候选和锁存状态，避免息屏/唤醒前后的残留状态干扰下一次按键识别。
 */
static void board_reset_touch_key_state(void)
{
    s_touch_lock_combo_latched = false;
    s_touch_lock_combo_confirm_cnt = 0;
    s_touch_lock_candidate_key = -1;
    s_touch_lock_candidate_tick = 0;
    s_touch_lock_release_inject_key = -1;
}

void audio_board_play_boot_release_guard_start(void)
{
    s_play_boot_guard_state = PLAY_BOOT_GUARD_WAIT_RELEASE;
    s_play_boot_release_tick = 0;
    board_reset_touch_key_state();
    ESP_LOGI(TAG, "PLAY boot guard started, wait for %d ms stable release",
             PLAY_BOOT_RELEASE_STABLE_MS);
}

/**
 * @brief 函数名称: board_oled_sleep_timer_cb
 * @param timerId 定时器 ID，本实现固定为 TIMER_ID_OLED_AUTO_SLEEP
 * @param arg 用户私有参数，当前未使用
 * @return 无
 * @note 作用: 在启用自动息屏且 OLED 已初始化时，向屏幕发送 0xAE 关屏命令，并进入等待按键唤醒状态。
 */
static void board_oled_sleep_timer_cb(U16 timerId, void *arg)
{
    (void)timerId;
    (void)arg;

    if (!s_oled_auto_sleep_enabled || !s_oled || s_oled_is_sleeping)
    {
        return;
    }

    board_reset_touch_key_state();
    s_oled_wake_swallow_active = false;
    oled_power_off(s_oled);
    s_oled_is_sleeping = true;
    ESP_LOGI(TAG, "OLED auto sleep timeout, send 0xAE to turn display off");
}

/**
 * @brief 函数名称: board_oled_restart_auto_sleep_timer
 * @param 无
 * @return 无
 * @note 作用: 在自动息屏已启用时，重新启动 3 分钟无按键息屏定时器；亮屏状态下每次有效触摸都会调用它。
 */
static void board_oled_restart_auto_sleep_timer(void)
{
    if (!s_oled_auto_sleep_enabled || !s_oled)
    {
        return;
    }

    if (FW_SetTimer(board_oled_sleep_timer_cb, TIMER_ID_OLED_AUTO_SLEEP, NULL, OLED_AUTO_SLEEP_TIMEOUT_MS) != pdPASS)
    {
        ESP_LOGW(TAG, "Failed to restart OLED auto sleep timer");
        return;
    }

    ESP_LOGD(TAG, "OLED auto sleep timer restarted");
}

/**
 * @brief 函数名称: board_oled_wake_from_sleep
 * @param 无
 * @return 无
 * @note 作用: 在 OLED 已息屏时向屏幕发送 0xAF 开屏命令，并开启吞键锁存，保证本次按键只唤醒不执行原功能。
 */
static void board_oled_wake_from_sleep(void)
{
    if (!s_oled || !s_oled_is_sleeping)
    {
        return;
    }

    board_reset_touch_key_state();
    oled_power_on(s_oled);
    s_oled_is_sleeping = false;
    s_oled_wake_swallow_active = true;
    board_oled_restart_auto_sleep_timer();
    ESP_LOGI(TAG, "OLED wake up, send 0xAF to turn display on and swallow current key press");
}

/**
 * @brief 函数名称: send_button_mask
 * @param 无
 * @return uint64_t 当前需要上报给按键外设层的按键位图
 * @note 作用: 采集底层触摸键状态，处理组合键、防抖，以及 OLED 息屏后首次按键只唤醒不透传的逻辑。
 */
uint64_t send_button_mask()
{
    uint64_t mask = 0;
    TickType_t current_tick = xTaskGetTickCount();
    bool prev_pressed = gpio_get_level(button_map[KEY_ID_PREV].gpio_num) == 0;
    bool next_pressed = gpio_get_level(button_map[KEY_ID_NEXT].gpio_num) == 0;
    bool play_pressed = gpio_get_level(button_map[KEY_ID_PLAY].gpio_num) == 0;
    bool any_key_pressed = prev_pressed || next_pressed || play_pressed;

    if (s_oled_auto_sleep_enabled)
    {
        if (s_oled_is_sleeping)
        {
            if (any_key_pressed)
            {
                board_oled_wake_from_sleep();
            }
            return 0;
        }

        if (s_oled_wake_swallow_active)
        {
            if (any_key_pressed)
            {
                return 0;
            }

            s_oled_wake_swallow_active = false;
            board_reset_touch_key_state();
            ESP_LOGI(TAG, "OLED wake swallow finished, key function restored");
            return 0;
        }

        if (any_key_pressed)
        {
            board_oled_restart_auto_sleep_timer();
        }
    }

    if (s_play_boot_guard_state != PLAY_BOOT_GUARD_READY)
    {
        if (play_pressed)
        {
            s_play_boot_guard_state = PLAY_BOOT_GUARD_WAIT_RELEASE;
            s_play_boot_release_tick = 0;
        }
        else if (s_play_boot_guard_state == PLAY_BOOT_GUARD_WAIT_RELEASE)
        {
            s_play_boot_guard_state = PLAY_BOOT_GUARD_CONFIRM_RELEASE;
            s_play_boot_release_tick = current_tick;
        }
        else if ((current_tick - s_play_boot_release_tick) >= pdMS_TO_TICKS(PLAY_BOOT_RELEASE_STABLE_MS))
        {
            s_play_boot_guard_state = PLAY_BOOT_GUARD_READY;
            s_play_boot_release_tick = 0;
            board_reset_touch_key_state();
            ESP_LOGI(TAG, "PLAY boot guard finished after stable release");
        }

        /* 本轮仍吞掉 PLAY，确保解除保护后必须重新按下才产生新事件 */
        play_pressed = false;
    }

    /* 上一拍若已补发短按按下，这一拍必须返回空掩码，让驱动自然生成释放事件 */
    if (s_touch_lock_release_inject_key >= 0)
    {
        s_touch_lock_release_inject_key = -1;
        return 0;
    }

    /* 组合键一旦建立后，只要 PLAY/NEXT 任意一键仍按下，就持续输出虚拟锁键 */
    if (s_touch_lock_combo_latched)
    {
        if (play_pressed || next_pressed)
        {
            return (1ULL << KEY_ID_TOUCH_LOCK);
        }

        /* 两个实体键都松开后再退出组合锁存，避免释放沿回落成单键事件 */
        s_touch_lock_combo_latched = false;
        s_touch_lock_combo_confirm_cnt = 0;
        s_touch_lock_candidate_key = -1;
        s_touch_lock_candidate_tick = 0;
        return 0;
    }

    /* 先处理 PLAY + NEXT 组合键，并清掉单键候选，避免误触发关机或切歌逻辑 */
    if (play_pressed && next_pressed)
    {
        if (s_touch_lock_combo_confirm_cnt < TOUCH_LOCK_COMBO_CONFIRM_CNT)
        {
            s_touch_lock_combo_confirm_cnt++;
            s_touch_lock_candidate_key = -1;
            s_touch_lock_candidate_tick = 0;
            return 0;
        }
        s_touch_lock_combo_latched = true;
        s_touch_lock_combo_confirm_cnt = 0;
        s_touch_lock_candidate_key = -1;
        s_touch_lock_candidate_tick = 0;
        return (1ULL << KEY_ID_TOUCH_LOCK);
    }
    s_touch_lock_combo_confirm_cnt = 0;

    /* PREV + NEXT 仍保持原有组合优先级，同时清掉 PLAY/NEXT 组合候选状态 */
    if (prev_pressed && next_pressed)
    {
        s_touch_lock_candidate_key = -1;
        s_touch_lock_candidate_tick = 0;
        return (1ULL << KEY_ID_BT);
    }

    /* PLAY/NEXT 单键先进入短观察窗，避免“先报 NEXT 再切入组合”导致误切歌 */
    if (play_pressed ^ next_pressed)
    {
        int current_candidate_key = next_pressed ? KEY_ID_NEXT : KEY_ID_PLAY;

        if (s_touch_lock_candidate_key != current_candidate_key)
        {
            s_touch_lock_candidate_key = current_candidate_key;
            s_touch_lock_candidate_tick = current_tick;
            return 0;
        }

        if ((current_tick - s_touch_lock_candidate_tick) < TOUCH_LOCK_GUARD_TICKS)
        {
            return 0;
        }
    }
    else if (!play_pressed && !next_pressed && s_touch_lock_candidate_key >= 0)
    {
        /* 观察窗内若已松手，则补发一次单键按下，下一拍空掩码会形成正常短按释放 */
        if ((current_tick - s_touch_lock_candidate_tick) < TOUCH_LOCK_GUARD_TICKS)
        {
            s_touch_lock_release_inject_key = s_touch_lock_candidate_key;
            mask = (1ULL << s_touch_lock_candidate_key);
            s_touch_lock_candidate_key = -1;
            s_touch_lock_candidate_tick = 0;
            return mask;
        }

        s_touch_lock_candidate_key = -1;
        s_touch_lock_candidate_tick = 0;
    }

    /* 未命中组合键时，按原始单键方式上报事件 */
    if (prev_pressed)
    {
        mask |= (1ULL << KEY_ID_PREV);
    }
    if (next_pressed)
    {
        mask |= (1ULL << KEY_ID_NEXT);
    }
    if (play_pressed)
    {
        mask |= (1ULL << KEY_ID_PLAY);
    }

    return mask;
}

esp_err_t audio_board_key_init(esp_periph_set_handle_t set)
{

    ESP_LOGI(TAG, "[*] vib board v1 audio_board_key_init");

    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = 0,
        .pull_up_en = 0,  // GPIO 36/37/38 are input-only, no internal pull-up
    };
    uint64_t pin_bit_mask = 0;
    for (int i = 0; i < BTN_HARD_NUM; i++)
    {
        if (button_map[i].gpio_num != -1)
        {
            pin_bit_mask |= (1ULL << button_map[i].gpio_num);
        }
    }
    io_conf.pin_bit_mask = pin_bit_mask;
    gpio_config(&io_conf);

    periph_key_cfg_t key_cfg = {
        .long_press_time_ms = 1000,
        .repeat_time_ms = 800,
        .long_long_press_time_ms = 3000,
        .key_num = BTN_HARD_NUM,
        .key_mask_get = send_button_mask,
    };
    key_periph = periph_key_init(&key_cfg);
    AUDIO_NULL_CHECK(TAG, key_periph, return ESP_ERR_ADF_MEMORY_LACK);
    return esp_periph_start(set, key_periph);
}

esp_err_t audio_board_key_deinit()
{
    if (key_periph == NULL)
    {
        ESP_LOGE(TAG, "key_periph is NULL");
        return ESP_FAIL;
    }
    esp_err_t err = esp_periph_destroy(key_periph);
    key_periph = NULL;
    return err;
}

esp_err_t audio_board_key_set_press_time(int key_id, int long_press_time_ms, int long_long_press_time_ms)
{
    if (key_periph == NULL)
    {
        ESP_LOGE(TAG, "key_periph is NULL");
        return ESP_FAIL;
    }
    return periph_key_set_press_time(key_periph, key_id, long_press_time_ms, long_long_press_time_ms);
}

static void _sdcard_init(void *arg)
{
    if (arg == NULL)
    {
        ESP_LOGE(TAG, "set is null!");
        return;
    }
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    esp_periph_set_handle_t set = (esp_periph_set_handle_t)arg;
    periph_sdcard_cfg_t sdcard_cfg = {
        .root = "/sdcard",
        // .card_detect_pin = get_sdcard_intr_gpio(),
        .card_detect_pin = GPIO_NUM_NC,
        .mode = SD_MODE_4_LINE,
    };
    sdcard_handle = periph_sdcard_init(&sdcard_cfg);
    esp_err_t ret = esp_periph_start(set, sdcard_handle);
    int retry_time = 5;
    bool mount_flag = false;
    while (retry_time--)
    {
        if (periph_sdcard_is_mounted(sdcard_handle))
        {
            mount_flag = true;
            break;
        }
        else
        {
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }
    if (mount_flag == false)
    {
        ESP_LOGE(TAG, "Sdcard mount failed");
    }

    vTaskDelete(NULL);
}

esp_err_t audio_board_sdcard_init(esp_periph_set_handle_t set)
{
    // if (mode >= SD_MODE_4_LINE) {
    //     ESP_LOGE(TAG, "Please select the correct sd mode!, current mode is %d", mode);
    //     return ESP_FAIL;
    // }
    ESP_LOGI(TAG, "[*]vib board V1: Start audio_board_sdcard_init ..");
    // _sdcard_init(set);
    if (pdPASS != xTaskCreatePinnedToCore((TaskFunction_t)_sdcard_init, "SDCard init", 1024 * 4, set, 1, NULL, ESP_TASK_MAIN_CORE))
    {
        ESP_LOGE(TAG, "Create sdcard init task failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t audio_board_sdcard_unmount()
{
    if (sdcard_handle == NULL)
    {
        ESP_LOGE(TAG, "sdcard_handle is NULL");
        return ESP_FAIL;
    }
    esp_err_t err = esp_periph_destroy(sdcard_handle);
    return err;
}

audio_board_handle_t audio_board_get_handle(void)
{
    return board_handle;
}

esp_err_t audio_board_deinit(audio_board_handle_t audio_board)
{
    esp_err_t ret = ESP_OK;
    ret = audio_hal_deinit(audio_board->audio_hal);
    audio_free(audio_board);
    board_handle = NULL;
    return ret;
}

esp_err_t audio_board_enable_sdcard_reader()
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_DEF_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << SDCARD_READER_EN_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(SDCARD_READER_EN_GPIO, 1);
    return ESP_OK;
}
esp_err_t audio_board_disable_sdcard_reader()
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_DEF_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << SDCARD_READER_EN_GPIO);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);
    gpio_set_level(SDCARD_READER_EN_GPIO, 0);
    return ESP_OK;
}

// #define GPIO_SDCARD_SEL  ((1ULL<<GPIO_NUM_2) | (1ULL<<GPIO_NUM_4)) | (1ULL<<GPIO_NUM_14) | (1ULL<<GPIO_NUM_15) | (1ULL<<GPIO_NUM_12) | (1ULL<<GPIO_NUM_13)
// esp_err_t audio_board_sdcard_reader_IO_config(){
//     gpio_config_t io_conf = {};
//     io_conf.intr_type = GPIO_INTR_DISABLE;
//     io_conf.mode = GPIO_MODE_DEF_DISABLE;
//     io_conf.pin_bit_mask = GPIO_SDCARD_SEL;
//     io_conf.pull_down_en = 0;
//     io_conf.pull_up_en = 0;
//     gpio_config(&io_conf);

//     // io_conf.pin_bit_mask = (1ULL<<SDCARD_READER_EN_GPIO);
//     // io_conf.mode = GPIO_MODE_DEF_INPUT;
//     // gpio_set_level(SDCARD_READER_EN_GPIO, 1);
//     // gpio_config(&io_conf);

//     return ESP_OK;
// }

// #define GPIO_SDCARD_
// esp_err_t audio_board_sdcard_reader_config(){
//     gpio_config_t io_conf = {};
//     io_conf.intr_type = GPIO_INTR_DISABLE;
//     io_conf.mode = GPIO_MODE_DEF_DISABLE;
//     io_conf.pin_bit_mask = GPIO_SDCARD_SEL;
//     io_conf.pull_down_en = 0;
//     io_conf.pull_up_en = 0;
//     gpio_config(&io_conf);
//     return ESP_OK;
// }

#include "esp_adc_cal.h"
#define BATTERY_ADC_UNIT BM_ADC_UNIT
#define BATTERY_ADC_CHANNEL BM_ADC_CHANNEL
#define BATTERY_ADC_ATTEN ADC_ATTEN_DB_11
#define DEFAULT_VREF 1100 // Use adc2_vref_to_gpio() to obtain a better estimate
// #define BATTERY_ADC_BITWIDTH    ADC_WIDTH_BIT_12

static adc_cali_handle_t adc_cali_handle = NULL;
static bool adc_calibrated = false;
static esp_adc_cal_characteristics_t *adc_chars;

static esp_err_t _get_battery_snap(periph_battery_snap_t *bat_snap)
{
    // get battery voltage
    chg_snapshot_t chg;
    chg_get_snap(&chg);
    // 每5秒打印一次
    static uint32_t last_log_time = 0;
    if (xTaskGetTickCount() - last_log_time > pdMS_TO_TICKS(5000))
    {
        last_log_time = xTaskGetTickCount();
        /* TS_STAT 温度状态字符串映射 */
        static const char *ts_stat_str[] = {
            "Normal",   /* 0: TH3-TH4 (default 15-35C) */
            "Cold",     /* 1: <TH1 */
            "Hot",      /* 2: >TH6 */
            "Cool",     /* 3: TH1-TH2 */
            "Warm",     /* 4: TH5-TH6 */
            "Precool",  /* 5: TH2-TH3 */
            "Prewarm",  /* 6: TH4-TH5 */
            "Fault"     /* 7: TS pin fault */
        };
        const char *ts_zone = (chg.ts_stat < 8) ? ts_stat_str[chg.ts_stat] : "?";
        
        ESP_LOGI(TAG, "chg_snapshot: present=%d, vbat_mV=%.2f, ibat_mA=%.2f, vsys_mV=%.2f, vbus_mV=%.2f, ts_percent=%.2f, ts_stat=%d(%s), charging=%d",
                 chg.present, chg.vbat_mV, chg.ibat_mA, chg.vsys_mV, chg.vbus_mV, chg.ts_percent, chg.ts_stat, ts_zone, chg.charging);

        if (chg.present)
        {
            chg_dump_debug();
        }
    }

    bat_snap->vbat = chg.vbat_mV;
    bat_snap->vdc = chg.vsys_mV;
    bat_snap->ischarging = (chg.vbus_stat != 0) || (chg.vbus_mV > 4300.0f);
    bat_snap->charge_active = chg.charging;
    bat_snap->is_full = bat_snap->ischarging &&
                        (!bat_snap->charge_active) &&
                        (chg.chg_stat == 0) &&
                        (chg.ibat_mA >= -20.0f) &&
                        (chg.ibat_mA <= 20.0f);

    return 0;
}

esp_err_t board_battery_init(esp_periph_set_handle_t set)
{
    ESP_LOGD(TAG, "on init");
    esp_err_t ret;
    
    /* Initialize I2C master bus if not already done */
    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = BOARD_I2C_NUM,
            .sda_io_num = BOARD_I2C_SDA,
            .scl_io_num = BOARD_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = true,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_i2c_bus));
        ESP_LOGI(TAG, "I2C master bus initialized: port=%d, SDA=%d, SCL=%d",
                 BOARD_I2C_NUM, BOARD_I2C_SDA, BOARD_I2C_SCL);
    }
    
    /* Add charger device to I2C bus */
    if (s_i2c_dev_charger == NULL) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BQ256XX_I2C_ADDR,
            .scl_speed_hz = BOARD_I2C_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_i2c_dev_charger));
    }
    
    chg_service_cfg_t chg_cfg = {
        .i2c_dev = s_i2c_dev_charger,
        .wdt_kick_ms = 5000,
    };
    ESP_ERROR_CHECK(chg_init(&chg_cfg));
    periph_battery_cfg_t cfg = {
        .intr_gpio = get_battery_charge_in_pin(),
        .get_snap = _get_battery_snap,
    };
    battery_handle = periph_battery_init(&cfg);
    ret = esp_periph_start(set, battery_handle);

    return ret;
}

// battery deinit
esp_err_t audio_board_battery_deinit()
{

    if (battery_handle == NULL)
    {
        ESP_LOGE(TAG, "battery_handle is NULL");
        return ESP_FAIL;
    }
    esp_err_t err = esp_periph_destroy(battery_handle);
    return err;
}



esp_err_t board_shut_down()
{
    ESP_LOGE(TAG, "board_shut_down");
    
    ESP_LOGE(TAG, "switch SDCARD TO USB");
    gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_27, 1);

    /* 关机前等待电源触摸键松开，避免断电后因按键仍保持按下而再次触发开机 */
    if (board_is_key_pressed(KEY_ID_PLAY))
    {
        TickType_t wait_start_tick = xTaskGetTickCount();
        TickType_t last_log_tick = wait_start_tick;

        ESP_LOGW(TAG, "Power key still pressed, wait release before power off");
        while (board_is_key_pressed(KEY_ID_PLAY))
        {
            TickType_t current_tick = xTaskGetTickCount();

            if ((current_tick - last_log_tick) >= pdMS_TO_TICKS(1000))
            {
                ESP_LOGW(TAG, "Waiting power key release...");
                last_log_tick = current_tick;
            }

            vTaskDelay(pdMS_TO_TICKS(20));
        }

        ESP_LOGI(TAG, "Power key released after %lu ms",
                 (unsigned long)(pdTICKS_TO_MS(xTaskGetTickCount() - wait_start_tick)));
    }

    gpio_set_level(GPIO_NUM_21, 0);
    vTaskDelay(500 / portTICK_PERIOD_MS);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    esp_restart();

    // // mx_stc_play_write(MX_PLAY_SHUTDOWN);
    // // mx_stc_write_data();
    return ESP_OK;
}

esp_err_t board_amplifier_normal_enable()
{
    ESP_LOGD(TAG, "board_amplifier_normal_enable");
    gpio_set_direction(GPIO_AMP_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_AMP_EN, 1);
    return ESP_OK;
}

esp_err_t board_amplifier_anti_distortion_enable()
{
    ESP_LOGD(TAG, "board_amplifier_anti_distortion_enable");
    gpio_set_direction(GPIO_AMP_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_AMP_EN, 1);
    esp_rom_delay_us(10);
    gpio_set_level(GPIO_AMP_EN, 0);
    esp_rom_delay_us(10);
    gpio_set_level(GPIO_AMP_EN, 1);

    esp_rom_delay_us(10);
    gpio_set_level(GPIO_AMP_EN, 0);
    esp_rom_delay_us(10);
    gpio_set_level(GPIO_AMP_EN, 1);

    return ESP_OK;
}

esp_err_t board_amplifier_disable()
{
    ESP_LOGD(TAG, "board_amplifier_normal_disable");
    gpio_set_level(GPIO_AMP_EN, 0);
    return ESP_OK;
}

esp_err_t board_oled_init(void)
{
    if (s_oled)
    {
        return ESP_OK;
    }

    /* Initialize I2C master bus if not already done */
    if (s_i2c_bus == NULL) {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = BOARD_I2C_NUM,
            .sda_io_num = BOARD_I2C_SDA,
            .scl_io_num = BOARD_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags.enable_internal_pullup = true,
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &s_i2c_bus));
        ESP_LOGI(TAG, "I2C master bus initialized: port=%d, SDA=%d, SCL=%d",
                 BOARD_I2C_NUM, BOARD_I2C_SDA, BOARD_I2C_SCL);
    }
    
    /* Add OLED device to I2C bus */
    if (s_i2c_dev_oled == NULL) {
        i2c_device_config_t dev_config = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = OLED_I2C_ADDR,
            .scl_speed_hz = BOARD_I2C_HZ,
        };
        ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_i2c_dev_oled));
    }

#ifdef OLED_RST_IO_NUM
    const int rst_gpio = OLED_RST_IO_NUM;
#else
    const int rst_gpio = -1;
#endif

    oled_cfg_t cfg = {
        .i2c_dev = s_i2c_dev_oled,
        .rst_gpio_num = rst_gpio,
        .flip_h = true,
        .flip_v = false,
        .use_charge_pump = true,
        .contrast = 0x7F,
        .col_offset = 34,
    };
    ESP_RETURN_ON_ERROR(oled_create(&cfg, &s_oled), TAG, "OLED create failed");

    oled_power_on(s_oled);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    oled_lock(s_oled);
    oled_clear(s_oled);
    // oled_draw_track_num(s_oled, 20, 12, 1);
    // oled_draw_text(s_oled, 0, 0, "VIB");
    oled_draw_battery(s_oled, 0, false); // BMS Service Will Update This To The Correct Value
    oled_draw_volume_bar(s_oled, 60);
    s_user_vol_displayed = 60;
    oled_update_locked(s_oled);
    oled_unlock(s_oled);

    return ESP_OK;
}

/**
 * @brief 函数名称: board_oled_set_auto_sleep_enabled
 * @param enable 是否启用 3 分钟无按键自动息屏，true 启用，false 禁用
 * @return 无
 * @note 作用: 控制板级 OLED 智能息屏总开关；禁用时停止定时器并退出吞键/息屏状态，启用时立即重新开始计时。
 */
void board_oled_set_auto_sleep_enabled(bool enable)
{
    s_oled_auto_sleep_enabled = enable;
    FW_ReleaseTimer(board_oled_sleep_timer_cb, TIMER_ID_OLED_AUTO_SLEEP);

    if (!enable)
    {
        s_oled_wake_swallow_active = false;
        board_reset_touch_key_state();
        if (s_oled && s_oled_is_sleeping)
        {
            oled_power_on(s_oled);
            ESP_LOGI(TAG, "OLED auto sleep disabled, send 0xAF to keep display on");
        }
        s_oled_is_sleeping = false;
        ESP_LOGI(TAG, "OLED auto sleep disabled");
        return;
    }

    s_oled_is_sleeping = false;
    s_oled_wake_swallow_active = false;
    board_reset_touch_key_state();
    board_oled_restart_auto_sleep_timer();
    ESP_LOGI(TAG, "OLED auto sleep enabled");
}

void dispayer_battery(int pct, bool outline_only)
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[dispayer_battery]");
    /* framebuffer 修改与发送放在同一把锁内，避免与后台动画任务交错形成混合帧 */
    oled_lock(s_oled);
    oled_draw_battery(s_oled, pct, outline_only);
    oled_update_locked(s_oled);
    oled_unlock(s_oled);
}

void displayer_volume(int pct)
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_volume]");
    /* 音量条区域在锁内完成绘制和发送，减少切换时瞬时乱显 */
    oled_lock(s_oled);
    oled_draw_volume_bar(s_oled, pct);
    oled_update_locked(s_oled);
    oled_unlock(s_oled);
}

void displayer_track_num(int num)
{
    if (!s_oled)
        return;

    /* 始终缓存最新曲目号，供锁图标 2 秒到期后恢复显示 */
    s_oled_track_num = num;
    if (app_player_is_a2dp_connected())
    {
        /* BT 已连接时禁止本地曲号回显，避免锁图标消失后恢复出本地编号 */
        oled_lock_img_set_restore_track(s_oled, -1);
        return;
    }
    oled_lock_img_set_restore_track(s_oled, num);

    /* 锁图标显示期间只更新恢复值，不覆盖当前图片 */
    if (oled_lock_img_is_active(s_oled))
        return;

    ESP_LOGI(TAG, "[displayer_track_num]");
    /* 曲目号刷新与整帧发送一起受保护，避免 2 秒恢复窗口内出现混合显示 */
    oled_lock(s_oled);
    oled_draw_track_num(s_oled, 20, 12, num);
    oled_update_locked(s_oled);
    oled_unlock(s_oled);
}

void displayer_track_clear()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_track_clear]");
    oled_lock(s_oled);
    oled_clear_track_num(s_oled, 20, 12);
    oled_update_locked(s_oled);
    oled_unlock(s_oled);
}

void displayer_text_bt()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_text_bt]");
    oled_lock(s_oled);
    oled_draw_text(s_oled, 20, 12, "BT");
    oled_update_locked(s_oled);
    oled_unlock(s_oled);
}
void displayer_bt_clear()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_bt_clear]");

    displayer_bt_blink_stop();
    oled_lock(s_oled);
    oled_clear_bt(s_oled, 0, 8);
    oled_update_locked(s_oled);
    oled_unlock(s_oled);
}

void displayer_bt_blink_slow()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_bt_blink_slow]");
    oled_bt_blink_start(s_oled, 0, 8, 1000);
}

void displayer_bt_blink_fast()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_bt_blink_fast]");
    oled_bt_blink_start(s_oled, 0, 8, 400);
}

void displayer_bt_blink_stop()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_bt_blink_stop]");
    oled_bt_blink_stop(s_oled, true);
}
void displayer_charge_blink_start()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_charge_blink_start]");
    oled_charge_blink_start(s_oled, 500);
}
void displayer_charge_blink_stop()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_charge_blink_stop]");
    oled_charge_blink_stop(s_oled);
}

void displayer_lock_img_show()
{
    if (!s_oled)
        return;

    ESP_LOGI(TAG, "[displayer_lock_img_show]");

    /* 图片放在曲目号区域中间位置，先清数字区域再显示图片 */
    oled_lock_img_show_timed(s_oled, 24, 10, 20, 12,
                             app_player_is_a2dp_connected() ? -1 : s_oled_track_num,
                             2000);
}

/**
 * @brief 函数名称: displayer_poweron
 * @param 无
 * @return 无
 * @note 作用: 对外提供统一的 OLED 开屏接口，内部发送 0xAF 命令恢复显示输出。
 */
void displayer_poweron()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_poweron] send 0xAF");

    oled_power_on(s_oled);
}

/**
 * @brief 函数名称: displayer_poweroff
 * @param 无
 * @return 无
 * @note 作用: 对外提供统一的 OLED 关屏接口，内部发送 0xAE 命令关闭显示输出。
 */
void displayer_poweroff()
{
    if (!s_oled)
        return;
    ESP_LOGI(TAG, "[displayer_poweroff] send 0xAE");

    oled_power_off(s_oled);
}
// void displayer_text_clear(){
//     oled_clear_text
// }
