/*
 * ESPRESSIF MIT License
 *
 * Copyright (c) 2020 <ESPRESSIF SYSTEMS (SHANGHAI) CO., LTD>
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

#include "board.h"
#include "esp_peripherals.h"
#include "sdkconfig.h"
#include "audio_mem.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "input_key_service.h"
#include "wifi_service.h"
#include "airkiss_config.h"
#include "smart_config.h"
#include "blufi_config.h"
#include "periph_adc_button.h"
#include "audio_embed_tone.h"

#include "audio_element.h"
#include "periph_key.h"

#include "app_control.h"
#include "app_player.h"
#include "app_bt.h"
#include "app_ble.h"
#include "audio_pipeline.h"
#include "esp_bt.h"
#include "app_gatt_device.h"
#include "esp_peripherals.h"
#include "periph_battery.h"
#include "fw_timer.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#define DEFAULT_VOLEME 60 //调节初始化音量大小0~100
#define WIFI_CONNECTED_BIT (BIT0)
#define WIFI_WAIT_CONNECT_TIME_MS (15000 / portTICK_PERIOD_MS)
#define POWER_OFF_TONE_URL TONE_URL_GREANPATCH
#define POWER_OFF_TONE_TIMEOUT_MS 3000

static const char *TAG = "APP_CONTROL";
esp_periph_set_handle_t set;

static int battery_level;
static app_battery_status_t battery_status = BATTERY_STATUS_DISCHARGING;

static int64_t s_last_activity_time = 0;
static bool s_ignore_play_shutdown = false;
static bool s_touch_locked = false;

#define TIMER_ID_AUTO_SHUTDOWN 1
#define AUTO_SHUTDOWN_CHECK_INTERVAL_MS 10000
#define AUTO_SHUTDOWN_INACTIVITY_SEC    (5 * 60)

static void app_shutdown_with_tone(void)
{
    esp_err_t tone_ret = app_player_play_shutdown_tone(POWER_OFF_TONE_URL,
                                                       POWER_OFF_TONE_TIMEOUT_MS);
    if (tone_ret != ESP_OK) {
        ESP_LOGW(TAG, "Shutdown tone did not finish cleanly: %s", esp_err_to_name(tone_ret));
    }

    app_poweroff_loader();
    board_shut_down();
}

static void auto_shutdown_check_cb(U16 timerId, void *arg)
{
    // Check BLE connection
    if (app_ble_is_connected()) {
        s_last_activity_time = esp_timer_get_time();
        FW_SetTimer(auto_shutdown_check_cb, TIMER_ID_AUTO_SHUTDOWN, NULL, AUTO_SHUTDOWN_CHECK_INTERVAL_MS);
        return;
    }

    // Check A2DP connection
    if (app_player_is_a2dp_connected()) {
        s_last_activity_time = esp_timer_get_time();
        FW_SetTimer(auto_shutdown_check_cb, TIMER_ID_AUTO_SHUTDOWN, NULL, AUTO_SHUTDOWN_CHECK_INTERVAL_MS);
        return;
    }

    // Check SD card playing
    if (app_player_is_sdcard_playing()) {
        s_last_activity_time = esp_timer_get_time();
        FW_SetTimer(auto_shutdown_check_cb, TIMER_ID_AUTO_SHUTDOWN, NULL, AUTO_SHUTDOWN_CHECK_INTERVAL_MS);
        return;
    }

    // Check charging
    if (battery_status == BATTERY_STATUS_CHARGING || battery_status == BATTERY_STATUS_FULL) {
        s_last_activity_time = esp_timer_get_time();
        FW_SetTimer(auto_shutdown_check_cb, TIMER_ID_AUTO_SHUTDOWN, NULL, AUTO_SHUTDOWN_CHECK_INTERVAL_MS);
        return;
    }

    // Check inactivity time
    int64_t now = esp_timer_get_time();
    if (now - s_last_activity_time > (int64_t)AUTO_SHUTDOWN_INACTIVITY_SEC * 1000 * 1000) {
        ESP_LOGW(TAG, "Auto shutdown triggered due to %d seconds of inactivity", AUTO_SHUTDOWN_INACTIVITY_SEC);
        app_shutdown_with_tone();
    } else {
        FW_SetTimer(auto_shutdown_check_cb, TIMER_ID_AUTO_SHUTDOWN, NULL, AUTO_SHUTDOWN_CHECK_INTERVAL_MS);
    }
}

void app_KeyEventHandler(Key_id_t key_id, periph_key_event_id_t key_type)
{
    s_last_activity_time = esp_timer_get_time();
    // ESP_LOGI(TAG, "[2] Initialize    bluetooth ");
    // esp_periph_handle_t bt_periph = app_bt_init(set);
    uint8_t has_rsp = 1;
    static Key_id_t last_key_id = -1;

    // 组合锁键单独处理，避免落入普通按键分支后触发原有功能
    if (key_id == KEY_ID_TOUCH_LOCK)
    {
        if (key_type == PERIPH_KEY_LONG_LONG_PRESS)
        {
            s_touch_locked = !s_touch_locked;
            if (s_touch_locked)
            {
                /* 仅在进入锁定时显示图片，解锁时保持原有逻辑不额外提示 */
                displayer_lock_img_show();
                //ESP_LOGW(TAG, "Touch lock enabled");
                printf("_______Touch lock enabled\n");
            }
            else
            {
                //ESP_LOGW(TAG, "Touch lock disabled");
                printf("_______Touch lock disabled\n");
            }
        }
        else
        {
            has_rsp = 0;
        }

        last_key_id = key_id;
        if (!has_rsp)
        {
            //ESP_LOGD(TAG, "no key action match");
            printf("_______no key action match\n");
        }
        return;
    }

    // 触摸锁定后，仅允许组合锁键继续进入上面的专用分支，其余按键全部拦截
    if (s_touch_locked)
    {
        if (key_type == PERIPH_KEY_SHORT_PRESSED)
        {
            /* 仅短按事件显示锁图标，可避免 100ms 内按下/松开/按下抖动时因释放事件产生额外误触发 */
            displayer_lock_img_show();
            //ESP_LOGW(TAG, "Touch locked");
            printf("_______Touch locked\n");
        }
        else
        {
            has_rsp = 0;
        }

        last_key_id = key_id;
        if (!has_rsp)
        {
            //ESP_LOGD(TAG, "no key action match");
            printf("_______no key action match\n");
        }
        return;
    }

    switch (key_id)
    {
    case KEY_ID_NEXT:
        if (last_key_id != KEY_ID_NEXT)
            break;
        if (key_type == PERIPH_KEY_SHORT_RELEASE)
        {
            // vTaskDelay(500/portTICK_PERIOD_MS);
            app_player_on_next();
            ESP_LOGW(TAG, "KEY ACTION: on_next");
            // app_player_embed_tone_play(TONE_URL_BLOOP_1);
        }
        else if (key_type == PERIPH_KEY_REPEAT || key_type == PERIPH_KEY_LONG_PRESS)
        {
            app_player_on_volume_up();
            ESP_LOGW(TAG, "KEY ACTION: volume_up");
            // app_player_embed_tone_play(TONE_URL_SHOOTING);
        }
        else
        {
            has_rsp = 0;
        }
        break;
    case KEY_ID_PREV:
        if (last_key_id != KEY_ID_PREV)
            break;
        if (key_type == PERIPH_KEY_SHORT_RELEASE)
        {
            // vTaskDelay(500/portTICK_PERIOD_MS);
            app_player_on_prev();            
            ESP_LOGW(TAG, "KEY ACTION: on_prev");
            // app_player_embed_tone_play(TONE_URL_BLOOP_2);
        }
        else if (key_type == PERIPH_KEY_REPEAT || key_type == PERIPH_KEY_LONG_PRESS)
        {
            app_player_on_volume_down();
            ESP_LOGW(TAG, "KEY ACTION: volume_down");
            
            // app_player_embed_tone_play(TONE_URL_SHOOTING);
        }
        else
        {
            has_rsp = 0;
        }
        break;
    // case KEY_ID_BT:
    //     if(key_type == PERIPH_KEY_LONG_RELEASE){
    //         // app_bt_dicoverable();
    //         // app_bt_start_or_stop();

    //         // if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED){
    //         //     ESP_LOGE(TAG, "BT ON");
    //         //     // app_bt_init(set);
    //         //     app_bt_start();
    //         // }else
    //         // // if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
    //         // {
    //         //     ESP_LOGE(TAG, "BT OFF");
    //         //     // app_bt_deinit();
    //         //     app_bt_stop();
    //         // }
    //     }
    //     else if( key_type == PERIPH_KEY_LONG_PRESS ){
    //         app_player_a2dp_disconnect();
    //         app_bt_dicoverable();
    //     }
    //     else if( key_type == PERIPH_KEY_LONG_LONG_PRESS ){
    //         app_player_embed_tone_play(TONE_URL_BLOOP_2);
    //         ESP_LOGW(TAG, "BT DISCOVERABLE");
    //         app_bt_recovery();
    //     }else{
    //         has_rsp = 0;
    //     }
    //     break;
    case KEY_ID_PLAY:
        if (key_type == PERIPH_KEY_LONG_LONG_RELEASE && s_ignore_play_shutdown)
        {
            s_ignore_play_shutdown = false;
            has_rsp = 0;
            break;
        }
        if (key_type == PERIPH_KEY_SHORT_RELEASE || key_type == PERIPH_KEY_LONG_RELEASE)
        {
            // 防止一次点击同时触发多个释放事件导致双次反转
            static uint32_t last_play_time = 0;
            uint32_t current_time = (uint32_t)(esp_timer_get_time() / 1000);
            if (current_time - last_play_time > 200) {
                last_play_time = current_time;
                // app_player_embed_tone_play(TONE_URL_BLOOP_1);
                app_player_on_play();
                ESP_LOGW(TAG, "KEY ACTION: play/pause");
            }
        }
        else if (key_type == PERIPH_KEY_LONG_LONG_PRESS)
        {
            if (s_ignore_play_shutdown)
            {
                has_rsp = 0;
                break;
            }
            app_shutdown_with_tone();
            ESP_LOGW(TAG, "KEY ACTION: shut_down");
        }
        else
        {
            has_rsp = 0;
        }

        /* code */
        break;
    case KEY_ID_BT:
        if (key_type == PERIPH_KEY_SHORT_PRESSED)
        {

            app_player_embed_tone_play(TONE_URL_BLOOP_2);
            ESP_LOGW(TAG, "KEY ACTION: BT START");
            app_bt_start();
        }
        else if (key_type == PERIPH_KEY_LONG_PRESS)
        {
            app_player_embed_tone_play(TONE_URL_GREANPATCH);
            app_player_a2dp_disconnect();
            ESP_LOGW(TAG, "KEY ACTION: BT DISCOVERABLE");
            app_bt_dicoverable();
        }
        else if (key_type == PERIPH_KEY_LONG_LONG_PRESS)
        {
            app_player_embed_tone_play(TONE_URL_MIXKIT_CLICK_ERROR);
            ESP_LOGW(TAG, "KEY ACTION: BT RECOVERY");
            app_bt_recovery();
        }
        else
        {
            has_rsp = 0;
        }
        break;
    default:
        has_rsp = 0;
        break;
    }
    last_key_id = key_id;
    if (!has_rsp)
    {
        ESP_LOGD(TAG, "no key action match");
    }
}

void delay_to_enable_sdcard_reader(void *arg)
{
    vTaskDelay(800);
    audio_board_enable_sdcard_reader();
    vTaskDelete(NULL);
}
void charger_cmd_handle(int cmd, int level)
{

    // ESP_LOGW(TAG, "[*] BATTERY EVENT");
    if (cmd == BM_EVENT_CHRG_IN)
    {
        ESP_LOGW(TAG, "[APP_BATTERY] CHRG_IN");
        battery_status = BATTERY_STATUS_CHARGING;
        ESP_ERROR_CHECK(board_oled_init());
        displayer_charge_blink_start();
    }
    if (cmd == BM_EVENT_CHRG_OUT)
    {
        ESP_LOGW(TAG, "[APP_BATTERY] CHRG_OUT");
        battery_status = BATTERY_STATUS_DISCHARGING;
        displayer_charge_blink_stop();

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
    if (cmd == BM_EVENT_BAT_LEVEL_CHANGE)
    {
        ESP_LOGW(TAG, "[APP_BATTERY] BAT_LEVEL_CHANGE, %d", (int)level);
        dispayer_battery(level, false);
        app_gatt_device_set_battery_level((int)level);
        if( battery_status == BATTERY_STATUS_DISCHARGING && battery_level == 16 && level == 15 ){ // level drop from 16 to 15
            app_player_embed_tone_play(TONE_URL_MIXKIT_CLICK_ERROR);
        }
        if( battery_status == BATTERY_STATUS_DISCHARGING && level <= 15 ){  // level drop and level is less than 16
            displayer_charge_blink_start();
        }
        battery_level = level;
    }
    if (cmd == BM_EVENT_BAT_LOW)
    {
        ESP_LOGE(TAG, "[APP_BATTERY] BAT_LOW, shut down");
        app_shutdown_with_tone();
    }
    if (cmd == BM_EVENT_CHRG_CMPL)
    {
        ESP_LOGW(TAG, "[APP_BATTERY] CHRG_CMPL");
        battery_status = BATTERY_STATUS_FULL;
        displayer_charge_blink_stop();
    }
}

void app_enty(void)
{

    // gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    // gpio_set_level(GPIO_NUM_21, 1);

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("A2DP_STREAM", ESP_LOG_ERROR);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_WARN);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_WARN);
    esp_log_level_set("SDCARD", ESP_LOG_DEBUG);
    esp_log_level_set("PERIPH_SDCARD", ESP_LOG_DEBUG);
    esp_log_level_set("PERIPH_SDCARD", ESP_LOG_DEBUG);
    esp_log_level_set("ESP_AUDIO_CTRL", ESP_LOG_WARN);
    esp_log_level_set("ESP_AUDIO_TASK", ESP_LOG_WARN);
    esp_log_level_set("PERIPH_KEY", ESP_LOG_DEBUG);
    esp_log_level_set("DRV8311", ESP_LOG_WARN);
    // esp_log_level_set("DOWNMIX", ESP_LOG_DEBUG);
    esp_log_level_set("i2s(legacy)", ESP_LOG_INFO);

    esp_log_level_set("PERIPH_BATTERY", ESP_LOG_WARN);
    esp_log_level_set("BT_KEYCTRL", ESP_LOG_DEBUG);
    // esp_log_level_set("PERIPH_BATTERY", ESP_LOG_DEBUG);
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGW(TAG, "[*]Running partition type %d subtype %d (offset 0x%08" PRIx32 ")",
             running->type, running->subtype, running->address);
    esp_app_desc_t running_app_info;
    if (esp_ota_get_partition_description(running, &running_app_info) == ESP_OK)
    {
        ESP_LOGW(TAG, "[*]Running firmware version: %s", running_app_info.version);
        ESP_LOGW(TAG, "[*]Build time %s %s", running_app_info.date, running_app_info.time);
    }

    FW_TimerInit();
    // 开机前启动的模块： 按键，电池和电源检测。

    ESP_LOGI(TAG, "[ 4 ] Set up esp set and event listener");
    esp_periph_config_t periph_cfg = DEFAULT_ESP_PERIPH_SET_CONFIG();
    // periph_cfg.task_core = app_core
    periph_cfg.extern_stack = true;
    set = esp_periph_set_init(&periph_cfg);

    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt_cfg.external_queue_size = 50;
    evt_cfg.internal_queue_size = 20;
    evt_cfg.queue_set_size = 20;
    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
    audio_board_key_init(set);

    if (ESP_OK != board_battery_init(set))
    {
        ESP_LOGE(TAG, "battery init fail");
    }

    audio_board_key_set_press_time(KEY_ID_BT, 3000, 8000);
    audio_board_key_set_press_time(KEY_ID_PLAY, 500, 1500);
    board_oled_set_auto_sleep_enabled(false);
    ESP_LOGI(TAG, "OLED auto sleep is disabled before key boot, charge-only wake keeps display on");
    audio_event_iface_set_listener(esp_periph_set_get_event_iface(set), evt);
    audio_event_iface_msg_t msg;

    bool ota_just_finished = false;
    bool ota_bt_conn = false;
    bool boot_by_key = false;
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &my_handle);
    if (err == ESP_OK) {
        uint8_t ota_done = 0;
        err = nvs_get_u8(my_handle, "ota_done", &ota_done);
        if (err == ESP_OK && ota_done == 1) {
            ota_just_finished = true;
            uint8_t bt_conn = 0;
            if (nvs_get_u8(my_handle, "ota_bt_conn", &bt_conn) == ESP_OK) {
                ota_bt_conn = bt_conn != 0;
            }
            ESP_LOGI(TAG, "OTA just finished, skipping key press wait, BT conn: %d", ota_bt_conn);
            // Clear the flag
            nvs_set_u8(my_handle, "ota_done", 0);
            nvs_commit(my_handle);
        }
        nvs_close(my_handle);
    }

    if (ota_just_finished) {
         ESP_LOGW(TAG, "OTA RESTART, DIRECT START UP");

         goto ota_finish;
    }
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_21, 0);
    while (1)
    {
        // 如果充电中, 则不会关机. 否则会掉电关机.
        // 收到长按开机。
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            continue;
        }
        if (msg.source_type == PERIPH_ID_BATTERY)
        {
            charger_cmd_handle(msg.cmd, (int)(msg.data));
        }
        if (msg.source_type == PERIPH_ID_KEY)
        {
            ESP_LOGI(TAG, "KEY_ID: %d, ACTION:%d ", (int)(msg.data), msg.cmd);
            if ((Key_id_t)(msg.data) == KEY_ID_PLAY && msg.cmd == PERIPH_KEY_LONG_LONG_PRESS)
            {
                ESP_LOGW(TAG, "LONG PRESS PLAY KEY, START UP");
                boot_by_key = true;
                break;
            }
        }
    }
ota_finish:
    // keep power on
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_21, 1);
    s_ignore_play_shutdown = boot_by_key;
    if (boot_by_key)
    {
        audio_board_play_boot_release_guard_start();
    }
    // 单独缩短 PLAY 关机阈值，保持现有开关机体验不变
    audio_board_key_set_press_time(KEY_ID_PLAY, 1000, 2000);
    // 显式设置组合锁键阈值，确保 PLAY + NEXT 需要按住 3 秒才会切换锁定状态
    audio_board_key_set_press_time(KEY_ID_TOUCH_LOCK, 1000, 3000);
    // start oled
    ESP_ERROR_CHECK(board_oled_init());
    board_oled_set_auto_sleep_enabled(true);
    ESP_LOGI(TAG, "OLED auto sleep is enabled after full boot");

    // 禁止USB读取 sdcard
    gpio_set_direction(GPIO_NUM_27, GPIO_MODE_OUTPUT);
    app_bt_init(set);
    app_gatt_device_set_battery_level(battery_level);

    // start ota server

    ESP_LOGI(TAG, "[1.0] Start audio codec chip");
    audio_board_handle_t board_handle = audio_board_init();
    if (board_handle == NULL)
    {
        ESP_LOGE(TAG, "ES8311 init fail");
        vTaskDelay(100 / portTICK_PERIOD_MS);
        // app_poweroff_loader();
    }
    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);

    if (ESP_OK != audio_board_sdcard_init(set))
    {
        ESP_LOGE(TAG, "sdcar init fail");
    }
    // AMP SET
    board_amplifier_anti_distortion_enable();

    app_player_init(board_handle);

    audio_hal_set_volume(board_handle->audio_hal, DEFAULT_VOLEME);
    audio_board_enable_volume_ui();
    ESP_LOGI(TAG, "[ 3 ] Create and start input key service");

    s_last_activity_time = esp_timer_get_time();
    FW_SetTimer(auto_shutdown_check_cb, TIMER_ID_AUTO_SHUTDOWN, NULL, AUTO_SHUTDOWN_CHECK_INTERVAL_MS);
    
    dispayer_battery(battery_level, false);
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    app_gatt_device_set_firmware_version(running_app_info.version);

#if 0 // old code
    audio_board_sdcard_reader_IO_config();
    // gpio_set_level(SDCARD_READER_EN_GPIO, 0);
    //ES8311 EN (audio ldo enable)
    gpio_set_direction(GPIO_NUM_21, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_21, 1);
    // task_delay_ms(100);

    
    
    //io 
    gpio_set_direction(get_sdcard_intr_gpio(), GPIO_MODE_INPUT);
    gpio_pullup_en(get_sdcard_intr_gpio());





    //for test
    app_bt_init(set);
    vTaskDelay(200/portTICK_PERIOD_MS);
    
    vTaskDelay(800/portTICK_PERIOD_MS);

    //reset the display
    board_display_set_bt_state(MX_BT_BREAK);
    displayer_track_num(0);
  







    // vTaskDelay(1000/portTICK_PERIOD_MS);
    // //reset esp32
    // esp_restart();


    //just for test
    // app_player_on_play();

    //LED
    gpio_set_direction(GPIO_NUM_37, GPIO_MODE_DEF_INPUT);
    gpio_set_level(GPIO_NUM_37, 0);

#endif

    app_player_embed_tone_play(TONE_URL_GREANPATCH);

    if (ota_just_finished && ota_bt_conn) {
        app_bt_start();
    }
    while (1)
    {
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            continue;
        }

        ESP_LOGW(TAG, "[*] source_type: %d, cmd: %d, data: %d", msg.source_type, msg.cmd, (int)msg.data);
        if (msg.source_type == PERIPH_ID_SDCARD)
        {
            if (msg.cmd == SDCARD_STATUS_UNMOUNTED || msg.cmd == SDCARD_STATUS_UNMOUNT_ERROR)
            {
                ESP_LOGW(TAG, "[*] SDCARD unmounted");
                app_player_sdcard_on_unmount();
                // audio_board_sdcard_reader_IO_config();
                // audio_board_disable_sdcard_reader();
                if (pdPASS != xTaskCreatePinnedToCore((TaskFunction_t)delay_to_enable_sdcard_reader, "SDCard reader", 4 * 1024, NULL, 1, NULL, ESP_TASK_MAIN_CORE))
                {
                    ESP_LOGE(TAG, "Create SDCard reader init task failed");
                }
            }
            if (msg.cmd == SDCARD_STATUS_MOUNTED)
            {
                ESP_LOGW(TAG, "[*] SDCARD mounted");
                app_player_on_mount();
            }
        }
        if (msg.source_type == PERIPH_ID_KEY)
        {
            app_KeyEventHandler((int)(msg.data), msg.cmd);
        }
        if (msg.source_type == PERIPH_ID_BATTERY)
        {
            charger_cmd_handle(msg.cmd, (int)(msg.data));
        }
    }
}
#include "esp_bt.h"
#include "esp_bt_main.h"
void app_poweroff_loader(void)
{
    ESP_LOGW(TAG, "Start system shutdown sequence...");

    ESP_LOGI(TAG, "Stopping active audio before power off...");
    app_player_prepare_shutdown();

    // 3. Stop Communication (Radio & Bus)
    ESP_LOGI(TAG, "Stopping Bluetooth...");
    esp_bluedroid_disable();
    esp_bt_controller_disable();
    // WiFi is not used/active in this snippet, but if it were: esp_wifi_stop();

    // 1. Protect Data (Data Safety)
    ESP_LOGI(TAG, "Unmounting SD Card...");
    // Stop player first to stop reading/writing
    app_player_sdcard_on_unmount(); 
    // Then unmount filesystem
    audio_board_sdcard_unmount();

    audio_board_battery_deinit();
    displayer_poweroff();

    // 2. Physical Safety (Physical Safety)
    // board_shut_down handles:
    // - audio_board_battery_deinit()
    // - displayer_poweroff()
    // - gpio_set_level(GPIO_NUM_21, 0) (Power control)
    // - esp_restart()
    ESP_LOGI(TAG, "Executing physical shutdown...");
    
}
