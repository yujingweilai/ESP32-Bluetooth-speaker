#include <string.h>
#include "esp_log.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "fatfs_stream.h"
#include "downmix.h"
#include "filter_resample.h"
#include "raw_stream.h"
#include "board.h"
#include "periph_button.h"

#include "freertos/event_groups.h"
#include "esp_bt.h"
#include "a2dp_stream.h"

#include "audio_embed_tone.h"
#include "audio_error.h"
#include "app_player.h"

#include "embed_flash_stream.h"

#include "audio_mem.h"
#include "audio_def.h"
#include "audio_thread.h"
#include "ringbuf.h"

#include "esp_decoder.h"
#include "app_bt.h"
#include "board.h"

#include "equalizer.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_bt.h"


#include <inttypes.h>
#include "app_player_sdcard.h"
#include "esp_a2dp_api.h"
#include "fw_timer.h"


static const char *TAG = "APP_PlAYER";
#define MIX_INDEX_NUMBER_SOURCE_FILE 2
#define MIX_INDEX_BASE_STREAM 0
#define MIX_INDEX_TONE_STREAM 1
#define SDCARD_BASE_STREAM_SILENCE_TIMEOUT_MS 20
#define SHUTDOWN_TONE_BASE_TIMEOUT_MS 20
#define SHUTDOWN_TONE_SETTLE_MS 30
#define SHUTDOWN_TONE_POLL_MS 20
// #define SAMPLERATE 44100
#define SAMPLERATE 48000
#define NUM_INPUT_CHANNEL 1
#define TRANSMITTIME 1000
// #define TRANSMITTIME 3000

#define NVS_EQ_NAMESPACE "eq_config"
#define NVS_EQ_KEY "eq_gains"
#define EQ_BANDS_NUM 10 // 均衡器频段数量

typedef struct
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t element_mixer;
    audio_element_handle_t element_equalizer;
    audio_element_handle_t element_i2s;
    int eq_gains[EQ_BANDS_NUM];
} mixer_handler_t;

typedef struct
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t element_a2dp_stream;
    audio_element_handle_t element_rsp_filter;
    audio_element_handle_t element_raw;

} a2dp_audio_handler_t;
typedef struct
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t element_flash_stream;
    audio_element_handle_t element_decoder;
    audio_element_handle_t element_rsp_filter;
    audio_element_handle_t element_raw;
} embed_audio_handler_t;
typedef struct
{
    esp_a2d_connection_state_t conn_state;
    esp_a2d_audio_state_t audio_state;
    esp_bd_addr_t remote_bda;
} app_bt_link_t;
static app_bt_link_t s_bt_link;

#define TONE_PLAYER_CMD_PLAY 0x100
#define TONE_PLAYER_CMD_CLEANUP 0x101  // Cleanup tone pipeline (from timer callback)
typedef struct
{
    audio_event_iface_handle_t evt;
    audio_thread_t tsk_handle;
    mixer_handler_t mixer;
    sdcard_player_t *sdcard_handler;
    embed_audio_handler_t tone_audio;
    a2dp_audio_handler_t a2dp_audio;
    void *current_base_pipeline;
    audio_board_handle_t board_handle;

    bool block_pause;
    int latest_tone_type;
    bool is_tone_playing;
    ringbuf_handle_t current_base_ringbuf;
    ringbuf_handle_t frozen_base_ringbuf;
    bool base_stream_read_frozen;
} app_player_handler_t;

static app_player_handler_t *s_player;
static int s_base_stream_timeout_override_ms = -1;
#define BASE_STREAM_FREEZE_RB_BLOCK_SIZE 256
#define BASE_STREAM_FREEZE_RB_BLOCK_COUNT 4

/* static function declare*/
static void _app_player_tone_play_latest();
static esp_err_t _app_player_prepare_base_for_shutdown_tone(void);
static esp_err_t _app_player_force_tone_play(uint8_t tone_type);
static int _get_base_stream_timeout_ms(void);
static void _refresh_base_stream_timeout(void);
static void _apply_base_stream_input_ringbuf(void);
/*------------------------------*/

#define TIMER_ID_CHECK_TONE_STATE 1
#define TIMER_ID_RESET_MIXER_PIPELINE 2
#define TIMER_ID_RECONNECT 3
void player_timer_callback(U16 id,  void *arg)
{
    switch(id)
    {
        case TIMER_ID_CHECK_TONE_STATE:
            // 检查 tone pipeline 是否仍在播放
            if(audio_element_get_state(s_player->tone_audio.element_flash_stream) != AEL_STATE_RUNNING
                || audio_element_get_state(s_player->tone_audio.element_decoder) != AEL_STATE_RUNNING
                || audio_element_get_state(s_player->tone_audio.element_rsp_filter) != AEL_STATE_RUNNING
                || audio_element_get_state(s_player->tone_audio.element_raw) != AEL_STATE_RUNNING
            )
            {
                // Tone 播放完成，发送事件到 event handler 任务处理清理工作
                // 避免在 Timer Daemon Task 中执行阻塞操作
                audio_event_iface_msg_t msg = {
                    .source_type = AUDIO_ELEMENT_TYPE_PLAYER,
                    .source = (void *)s_player,
                    .cmd = TONE_PLAYER_CMD_CLEANUP,
                    .data = NULL,
                    .data_len = 0
                };
                if (audio_event_iface_sendout(s_player->evt, &msg) != ESP_OK) {
                    ESP_LOGW(TAG, "[player_timer_callback] Failed to send cleanup event");
                }
                FW_ReleaseTimer(player_timer_callback, TIMER_ID_CHECK_TONE_STATE);
            } else {
                // 仍在播放，继续轮询检查
                FW_SetTimer(player_timer_callback, TIMER_ID_CHECK_TONE_STATE, 0, 300);
            }
            break;
        case TIMER_ID_RESET_MIXER_PIPELINE:
            // Reset and restart mixer pipeline after it finished due to input data exhaustion
            ESP_LOGI(TAG, "[player_timer_callback] Resetting mixer pipeline");
            audio_pipeline_reset_ringbuffer(s_player->mixer.pipeline);
            audio_pipeline_reset_elements(s_player->mixer.pipeline);
            audio_pipeline_change_state(s_player->mixer.pipeline, AEL_STATE_INIT);
            audio_pipeline_run(s_player->mixer.pipeline);
            _refresh_base_stream_timeout();
            break;
        case TIMER_ID_RECONNECT:
            // 蓝牙重连逻辑
            {
                // 获取蓝牙的的可连接可发现
                bool is_connectable = app_bt_get_connectable();
                esp_bd_addr_t *bdaddr = app_bt_get_last_connected_bdaddr();
                if (!is_connectable)
                {
                    // 每10秒发一次log：
                    if (xTaskGetTickCount() / pdMS_TO_TICKS(1024) % 10 == 0)
                    {
                        ESP_LOGD(TAG, "[reconnect] BT is not connectable, restarting reconnect timer, remote_bda: " ESP_BD_ADDR_STR "", ESP_BD_ADDR_HEX(bdaddr[0]));
                    }
                    FW_SetTimer(player_timer_callback, TIMER_ID_RECONNECT, NULL, 1000); // 1秒后再次检查
                    return;
                }

                if (esp_bt_gap_get_bond_device_num() > 0)
                {
                    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
                    {
                        ESP_LOGE(TAG, "[reconnect] Bluetooth stack is not enabled");
                    }
                    else
                    {
                        ESP_LOGD(TAG, "[reconnect] -------Last connected device address: " ESP_BD_ADDR_STR "---------",
                                 ESP_BD_ADDR_HEX(bdaddr[0]));
                        ESP_LOGW(TAG, "[reconnect] A2DP[player_timer_callback] reconnect");
                        esp_a2d_sink_connect(bdaddr[0]);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "A2DP [player_timer_callback] no bond device");
                }
                // 重连定时器在连接成功或主动停止时才释放，这里不释放
            }
            break;
        default:
            break;
    }
}

static int _get_base_stream_timeout_ms(void)
{
    /*
     * Mixer 读取 base stream 的超时策略：
     * 1. 当前没有有效基础流时，直接返回 0，避免无意义等待。
     * 2. 当前基础流是 SD 卡，默认使用 0ms，尽快暴露“无数据”状态给上层守护逻辑。
     * 3. 若 SD 卡侧主动开启 silence wait，则改用覆盖超时，允许 mixer 短暂静音等待补仓。
     * 4. 蓝牙等流保持较温和的 50ms 超时，减少瞬时抖动带来的空读。
     */
    if (s_player == NULL || s_player->current_base_pipeline == NULL) {
        return 0;
    }
    if (s_base_stream_timeout_override_ms >= 0 &&
        s_player->current_base_pipeline == s_player->sdcard_handler) {
        return s_base_stream_timeout_override_ms;
    }
    if (s_player->current_base_pipeline == s_player->sdcard_handler) {
        return 0;
    }
    return 50;
}

static void _refresh_base_stream_timeout(void)
{
    /* 将最新的超时策略重新同步到 mixer 的 base stream 输入口。 */
    if (s_player == NULL || s_player->mixer.element_mixer == NULL) {
        return;
    }
    downmix_set_input_rb_timeout(s_player->mixer.element_mixer,
                                 _get_base_stream_timeout_ms(),
                                 MIX_INDEX_BASE_STREAM);
}

static void _apply_base_stream_input_ringbuf(void)
{
    ringbuf_handle_t target_rb = NULL;

    /*
     * 基础流 ringbuf 的切换逻辑：
     * 1. 正常情况下，mixer 直接读取当前真实的基础流 ringbuf。
     * 2. 进入 read_freeze 后，如果当前源是 SD 卡，则改挂到 frozen ringbuf。
     * 3. frozen ringbuf 本身不再承接真实 PCM 数据，相当于“冻结下游消费口”，
     *    让上游继续往真实输出缓存里补仓，实现预缓存/低水位等待。
     */
    if (s_player == NULL || s_player->mixer.element_mixer == NULL) {
        return;
    }

    target_rb = s_player->current_base_ringbuf;
    if (s_player->base_stream_read_frozen &&
        s_player->current_base_pipeline == s_player->sdcard_handler &&
        s_player->frozen_base_ringbuf != NULL) {
        target_rb = s_player->frozen_base_ringbuf;
    }

    if (target_rb != NULL) {
        downmix_set_input_rb(s_player->mixer.element_mixer, target_rb, MIX_INDEX_BASE_STREAM);
    }
}

/*
 * 控制 base stream 的“静音等待”模式。
 * 设计目的：
 * 1. 不直接改变 pipeline 的 RUNNING/PAUSED 状态，避免打乱上层状态机。
 * 2. 仅通过调小 mixer 读超时，让下游在短超时内进入“缺数则静音等待”的行为。
 * 3. 主要用于 SD 卡起播预缓存、软切歌、低水位保护恢复前的等待阶段。
 */
esp_err_t app_player_set_base_stream_silence_wait(bool enable)
{
    static bool last_enable_state = false;
    static bool has_set_once = false;

    if (s_player == NULL || s_player->mixer.element_mixer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (has_set_once && last_enable_state == enable) {
        return ESP_OK;
    }

    if (enable) {
        /* 只允许 SD 卡基础流进入静音等待，避免影响蓝牙等其他输入路径。 */
        if (s_player->current_base_pipeline != s_player->sdcard_handler) {
            return ESP_OK;
        }
        s_base_stream_timeout_override_ms = SDCARD_BASE_STREAM_SILENCE_TIMEOUT_MS;
    } else {
        s_base_stream_timeout_override_ms = -1;
    }

    last_enable_state = enable;
    has_set_once = true;

    _refresh_base_stream_timeout();
    ESP_LOGI(TAG, "[base_stream_silence_wait] enable=%d timeout=%d",
             enable ? 1 : 0,
             _get_base_stream_timeout_ms());
    return ESP_OK;
}

/*
 * 控制 base stream 的“读取冻结”模式。
 * 设计目的：
 * 1. 不暂停整个 pipeline，只冻结 mixer 对真实基础流的消费动作。
 * 2. 冻结后，上游解码/重采样仍可继续把数据写入真实输出 RB，实现缓存补仓。
 * 3. 常用于 SD 卡起播预缓存、软切歌静音过渡、低水位等待恢复。
 */
esp_err_t app_player_set_base_stream_read_freeze(bool enable)
{
    if (s_player == NULL || s_player->mixer.element_mixer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_player->base_stream_read_frozen == enable) {
        return ESP_OK;
    }

    if (enable) {
        /* 同样只对 SD 卡链路生效，避免误冻结蓝牙基础流。 */
        if (s_player->current_base_pipeline != s_player->sdcard_handler) {
            return ESP_OK;
        }
        if (s_player->frozen_base_ringbuf == NULL) {
            return ESP_ERR_INVALID_STATE;
        }
        /* 标记冻结后，_apply_base_stream_input_ringbuf() 会把 mixer 输入切到 frozen ringbuf。 */
        s_player->base_stream_read_frozen = true;
    } else {
        /* 解除冻结后，mixer 重新接回真实基础流 ringbuf。 */
        s_player->base_stream_read_frozen = false;
    }

    _apply_base_stream_input_ringbuf();
    ESP_LOGI(TAG, "[base_stream_read_freeze] enable=%d frozen=%d",
             enable ? 1 : 0,
             s_player->base_stream_read_frozen ? 1 : 0);
    return ESP_OK;
}

esp_err_t app_player_get_user_volume(int *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_player == NULL || s_player->board_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return audio_board_get_user_voice_volume(volume);
}

esp_err_t app_player_set_base_stream_low_water_duck(bool enable, int limit)
{
    /*
     * 低水位 duck 的本质是“运行时音量上限钳制”：
     * 1. enable=false：取消低水位音量限制，恢复正常用户音量控制。
     * 2. enable=true ：将当前输出音量上限限制在 limit，用于渐弱/渐强过程。
     * 3. 该接口本身不判断低水位，只执行音量限制动作，时序由 SD 卡侧守护控制。
     */
    if (s_player == NULL || s_player->board_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!enable) {
        return audio_board_set_runtime_voice_limit(-1);
    }

    return audio_board_set_runtime_voice_limit(limit);
}



// void play_state_check_timer_callback(void* arg)
// {
//     audio_element_state_t state = (audio_element_state_t)(intptr_t)arg;
//     ESP_LOGI(TAG, "[play_state_check_timer]timer_callback, id%d", state);
//     if(s_player->current_base_pipeline == s_player->sdcard_audio.pipeline){
//         // if(ESP_OK != audio_pipeline_check_items_state(s_player->sdcard_audio.pipeline, s_player->sdcard_audio.element_fatfs, state))
//         // {
//         //     ESP_LOGE(TAG, "state no aligned");

//         if(state == AEL_STATE_RUNNING){
//             if(audio_pipeline_get_state(s_player->sdcard_audio.pipeline) != state){
//                 ESP_LOGW(TAG, "[play_state_check_timer]sdcard_audio pipeline state no aligned");
//                 audio_pipeline_stop(s_player->sdcard_audio.pipeline);
//                 audio_pipeline_wait_for_stop(s_player->sdcard_audio.pipeline);

//                 audio_pipeline_terminate(s_player->sdcard_audio.pipeline);
//                 audio_pipeline_change_state(s_player->sdcard_audio.pipeline, AEL_STATE_INIT);
//                 audio_pipeline_reset_ringbuffer(s_player->sdcard_audio.pipeline);
//                 audio_pipeline_reset_elements(s_player->sdcard_audio.pipeline);
//                 audio_pipeline_run(s_player->sdcard_audio.pipeline);
//             }
//             else
//             {
//                 bool all_running = true;
//                 if(audio_element_get_state(s_player->sdcard_audio.element_fatfs) != state){
//                     ESP_LOGW(TAG, "[play_state_check_timer]fatfs state no aligned");
//                     // audio_element_change_state(s_player->sdcard_audio.element_fatfs, state);
//                     // audio_element_run(s_player->sdcard_audio.element_fatfs);
//                     all_running = false;
//                 }
//                 if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_fatfs)){
//                     ESP_LOGW(TAG, "[play_state_check_timer]fatfs not running");
//                     // audio_element_resume(s_player->sdcard_audio.element_fatfs, 0 , 200);
//                     all_running = false;
//                 }
//                 if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_decoder)){
//                     ESP_LOGW(TAG, "[play_state_check_timer]decoder not running");
//                     // audio_element_resume(s_player->sdcard_audio.element_decoder, 0 , 200);
//                     all_running = false;
//                 }
//                 if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_rsp_filter)){
//                     ESP_LOGW(TAG, "[play_state_check_timer]rsp_filter not running");
//                     // audio_element_resume(s_player->sdcard_audio.element_rsp_filter, 0 , 200);
//                     all_running = false;
//                 }
//                 if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_raw)){
//                     ESP_LOGW(TAG, "[play_state_check_timer]raw not running");
//                     // audio_element_resume(s_player->sdcard_audio.element_raw, 0 , 200);
//                     all_running = false;
//                 }
//                 if( !all_running ){
//                     ESP_LOGW(TAG, "[play_state_check_timer]some element not running");
//                     audio_pipeline_stop(s_player->current_base_pipeline);
//                     audio_pipeline_wait_for_stop(s_player->current_base_pipeline);
//                     audio_pipeline_terminate(s_player->current_base_pipeline);

//                     audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_INIT);
//                     audio_pipeline_reset_ringbuffer(s_player->current_base_pipeline);
//                     audio_pipeline_reset_elements(s_player->current_base_pipeline);
//                     // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
//                     audio_pipeline_run(s_player->current_base_pipeline);
//                 }
//             }
//             //check mixer state
//             if( audio_pipeline_get_state(s_player->mixer.pipeline) != state ){
//                 audio_pipeline_stop(s_player->mixer.pipeline);
//                 audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
//                 audio_pipeline_terminate(s_player->mixer.pipeline);
//                 audio_pipeline_change_state(s_player->mixer.pipeline, AEL_STATE_INIT);
//                 audio_pipeline_run(s_player->mixer.pipeline);
//             }
//             else{
//                 if(AEL_STATE_RUNNING != audio_element_get_state(s_player->mixer.element_mixer) || AEL_STATE_RUNNING != audio_element_get_state(s_player->mixer.element_equalizer) || AEL_STATE_RUNNING != audio_element_get_state(s_player->mixer.element_i2s)){
//                     audio_pipeline_stop(s_player->mixer.pipeline);
//                     audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
//                     audio_pipeline_terminate(s_player->mixer.pipeline);
//                     audio_pipeline_change_state(s_player->mixer.pipeline, AEL_STATE_INIT);
//                     audio_pipeline_run(s_player->mixer.pipeline);
//                 }
//             }
//         }
//     }
//     if(s_player->block_pause) s_player->block_pause = false;

//     s_player->sdcard_audio.operator_lock = false;
// }

#if 0
void app_palyer_event_handler(void *pv)
{
    app_player_handler_t *player = (app_player_handler_t *)pv;
    while (1)
    {
        /* Handle event interface messages from pipeline
           to set music info and to advance to the next song
        */
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(s_player->evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[ app_palyer_event_handler] Event interface error : %d", ret);
            continue;
        }

        ESP_LOGD(TAG, "[app_palyer_event_handler] [%s], cmd=%d, data=%d", audio_element_get_tag(msg.source), msg.cmd, (int)msg.data );
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT)
        {
            // Mixer state report
            if(msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
                (msg.source == (void *)(s_player->mixer.element_i2s) 
                || msg.source == (void *)(s_player->mixer.element_mixer) 
                || msg.source == (void *)(s_player->mixer.element_mixer) ) ){
                audio_element_status_t el_status = (audio_element_status_t)msg.data;

                // ESP_LOGD(TAG,"[ * ] i2s state: %d", el_status);
                // if(s_player->current_base_pipeline != s_player->sdcard_audio.pipeline){
                    
                // }
                // else 
                if(el_status == AEL_STATUS_STATE_FINISHED){
                    ESP_LOGW(TAG, "[app_palyer_event_handler]Mixer finished");
                    audio_pipeline_reset_ringbuffer(s_player->mixer.pipeline);
                    audio_pipeline_reset_elements(s_player->mixer.pipeline);
                    audio_pipeline_change_state(s_player->mixer.pipeline, AEL_STATE_INIT);
                    // audio_pipeline_resume(s_player->mixer.pipeline);
                    audio_pipeline_run(s_player->mixer.pipeline);
                }
                
            }
            // Advance to the next song when previous finishes
            if (msg.source == (void *)(s_player->sdcard_audio.element_rsp_filter) && msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
            {
                audio_element_state_t el_state = audio_element_get_state(s_player->sdcard_audio.element_rsp_filter);
                ESP_LOGD(TAG,"[app_palyer_event_handler] rsp_filte state: %d", el_state);
                if(s_player->current_base_pipeline!= s_player->sdcard_audio.pipeline){
                    
                }
                else if (el_state == AEL_STATE_FINISHED)
                {
                    ESP_LOGI(TAG, "[app_palyer_event_handler] Music Finished");
                    if (s_player->current_base_pipeline == s_player->sdcard_audio.pipeline)
                    {
                        ESP_LOGI(TAG, "[app_palyer_event_handler] SDcard music next");
                        char *url = NULL;
                        sdcard_list_next(s_player->sdcard_audio.sdcard_list, 1, &url);
                        ESP_LOGI(TAG, "[app_palyer_event_handler]URL: %s", url);
                        /* In previous versions, audio_pipeline_terminal() was called here. It will close all the element task and when we use
                         * the pipeline next time, all the tasks should be restarted again. It wastes too much time when we switch to another music.
                         * So we use another method to achieve this as below.
                         */
                        audio_element_set_uri(s_player->sdcard_audio.element_fatfs, url);
                        audio_pipeline_reset_ringbuffer(s_player->sdcard_audio.pipeline);
                        audio_pipeline_reset_elements(s_player->sdcard_audio.pipeline);
                        audio_pipeline_change_state(s_player->sdcard_audio.pipeline, AEL_STATE_INIT);
                        // audio_pipeline_resume(s_player->mixer.pipeline);
                        audio_pipeline_run(s_player->sdcard_audio.pipeline);
                    }
                }
            }
            // Print music info when receive music info from decoder
            if(msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO){
                audio_element_info_t music_info = {0};
                audio_element_getinfo(msg.source, &music_info);
                ESP_LOGI(TAG, "[app_palyer_event_handler] Received music info from [%s], sample_rates=%d, bits=%d, ch=%d",
                            audio_element_get_tag(msg.source),music_info.sample_rates, music_info.bits, music_info.channels);                            
                if(msg.source == (void *)(s_player->a2dp_audio.element_a2dp_stream) ){
                    rsp_filter_set_src_info(s_player->a2dp_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);                
                }
                if (msg.source == (void *)s_player->sdcard_audio.element_decoder){
                    rsp_filter_set_src_info(s_player->sdcard_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);
                }
                if (msg.source == (void *)s_player->tone_audio.element_decoder){
                    rsp_filter_set_src_info(s_player->tone_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);
                }
            }
            /* Stop when the tone pipeline element receives stop event */
            // if ( msg.source == (void *)s_player->tone_audio.element_rsp_filter
            //     && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (((int)msg.data == AEL_STATUS_STATE_STOPPED)
            //             || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
            //     downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_SWITCH_OFF);
            //     downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 0, 1);
            //     audio_pipeline_stop(s_player->tone_audio.pipeline);
            //     audio_pipeline_wait_for_stop(s_player->tone_audio.pipeline);
            //     audio_pipeline_terminate(s_player->mixer.pipeline);
            //     audio_pipeline_reset_ringbuffer(s_player->tone_audio.pipeline);
            //     audio_pipeline_reset_elements(s_player->tone_audio.pipeline);
            //     // audio_pipeline_check_items_state(s_player->tone_audio.pipeline, s_player->tone_audio.element_flash_stream,AEL_STATE_INIT);
            //     downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS); //will lead to alwas in bypass mode
            //     ESP_LOGI(TAG, "New come music stope`d or finsihed");
            // }

            // Update Play state, set display
            if((msg.source == (void *)s_player->a2dp_audio.element_a2dp_stream && s_player->current_base_pipeline == s_player->a2dp_audio.pipeline) 
                || (msg.source == (void *)s_player->sdcard_audio.element_fatfs && s_player->current_base_pipeline == s_player->sdcard_audio.pipeline)){
                if(msg.cmd == AEL_MSG_CMD_REPORT_STATUS ){
                    audio_element_status_t el_state = (int)msg.data;
                    if(el_state == AEL_STATUS_STATE_RUNNING){
                        board_display_set_play_state(MX_PLAY_PLAY);
                    }
                    else{
                        board_display_set_play_state(MX_PLAY_PAUSE);
                    }
                }
            }
            // Update song number
            if(msg.source == (void *)s_player->sdcard_audio.element_fatfs && msg.cmd == AEL_MSG_CMD_REPORT_STATUS){
                audio_element_status_t el_state = (int)msg.data;
                if(el_state == AEL_STATUS_STATE_RUNNING){
                    uint16_t num = sdcard_list_get_url_id(s_player->sdcard_audio.sdcard_list);
                    displayer_track_num(num);
                }
            
            }
            //audio element err
            if(msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (int)(msg.data) < AEL_STATUS_ERROR_UNKNOWN){
                ESP_LOGE(TAG, "[app_palyer_event_handler] audio element err");
                if(msg.source == s_player->sdcard_audio.element_decoder){
                    ESP_LOGE(TAG, "[app_palyer_event_handler] sdcard_audio.element_decoder err,  %d", (int)msg.data);
                    app_player_on_next();       
                }
                if(msg.source == s_player->sdcard_audio.element_fatfs){
                    ESP_LOGE(TAG, "[app_palyer_event_handler] sdcard_audio.element_fatfs err,  %d", (int)msg.data);
                    app_player_on_next();  
                }
                if(msg.source == s_player->sdcard_audio.element_raw){
                    ESP_LOGE(TAG, "[app_palyer_event_handler] sdcard_audio.element_raw err, %d", (int)msg.data);
                    app_player_on_next();  
                }
                if(msg.source == s_player->sdcard_audio.element_rsp_filter){
                    ESP_LOGE(TAG, "[app_palyer_event_handler] sdcard_audio.element_rsp_filter err, %d", (int)msg.data); 
                    app_player_on_next();  
                }
            }
        }
        if ( msg.source == (void *)s_player->tone_audio.element_decoder
            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && ( /*((int)msg.data == AEL_STATUS_STATE_STOPPED)
                    || */((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                ESP_LOGI(TAG, "[app_palyer_event_handler] Tone music stoped or finsihed1");
            downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_SWITCH_OFF);
            downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 0, 1);
            // FIX: 先 abort ringbuffer 解除可能阻塞的元素任务
            audio_element_abort_input_ringbuf(s_player->tone_audio.element_decoder);
            audio_element_abort_output_ringbuf(s_player->tone_audio.element_decoder);
            audio_element_abort_input_ringbuf(s_player->tone_audio.element_rsp_filter);
            audio_element_abort_output_ringbuf(s_player->tone_audio.element_rsp_filter);
            audio_element_abort_input_ringbuf(s_player->tone_audio.element_raw);
            audio_pipeline_stop(s_player->tone_audio.pipeline);
            audio_pipeline_wait_for_stop(s_player->tone_audio.pipeline);
            audio_pipeline_terminate(s_player->tone_audio.pipeline);
            audio_pipeline_reset_ringbuffer(s_player->tone_audio.pipeline);
            audio_pipeline_reset_elements(s_player->tone_audio.pipeline);
            // audio_pipeline_check_items_state(s_player->tone_audio.pipeline, s_player->tone_audio.element_flash_stream,AEL_STATE_INIT);
            downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS); //will lead to alwas in bypass mode
            ESP_LOGI(TAG, "[app_palyer_event_handler] Tone music stoped or finsihed2");
        }
    }
}
#endif

// void set_play_state_check_timer(audio_element_state_t state){

//     s_player->sdcard_audio.operator_lock = true;
//     if(s_player->state_check_timer != NULL){
//         esp_timer_stop(s_player->state_check_timer);
//         esp_timer_delete(s_player->state_check_timer);
//     }
//     esp_timer_create_args_t timer_args = {
//         .callback = play_state_check_timer_callback,
//         .arg = (void*)(intptr_t)state,
//         .name = "state_check_timer",
//         .dispatch_method = ESP_TIMER_TASK
//     };
//     esp_timer_create(&timer_args, &s_player->state_check_timer);
//     esp_timer_start_once(s_player->state_check_timer, 1500 * 1000);

// }

// audio_event_iface_handle_t evt;
// audio_pipeline_handle_t player->mixer.pipeline;
// audio_element_handle_t player->mixer.element_mixer;

// audio_pipeline_handle_t player->current_base_pipeline = NULL;

// audio_pipeline_handle_t player->sdcard_audio.pipeline;
// audio_element_handle_t player->sdcard_audio.element_raw = NULL;
// audio_element_handle_t player->sdcard_audio.element_fatfs = NULL;

// audio_pipeline_handle_t player->tone_audio.pipeline;

// audio_element_handle_t player->tone_audio.element_raw = NULL;
// audio_element_handle_t player->tone_audio.element_decoder= NULL;
// audio_element_handle_t player->tone_audio.element_flash_stream =NULL;
// audio_element_handle_t player->tone_audio.element_rsp_filter = NULL;

// void app_sdcard_scan(){

//     ESP_LOGI(TAG, "[1.2] Set up a sdcard playlist and scan sdcard music save to it");

//     if(s_player->sdcard_audio.sdcard_list != NULL){
//         sdcard_list_destroy(s_player->sdcard_audio.sdcard_list);
//     }
//     if(ESP_OK != sdcard_list_create(&(s_player->sdcard_audio.sdcard_list))){
//         ESP_LOGE(TAG,"[app_player_sdcard_init] Create sdcard_list fail");
//         return ;
//     }

//     for(uint8_t i=0;i<10;i++){
//         if(ESP_OK != sdcard_scan(sdcard_url_save_cb, "/sdcard", 0, (const char *[]) {"mp3", "m4a", "flac", "ogg", "opus", "amr", "ts", "aac", "wav"}, 9, s_player->sdcard_audio.sdcard_list)){
//             ESP_LOGW(TAG, "sdcard scan fail retry..");
//             vTaskDelay(500/portTICK_PERIOD_MS);
//         }else{
//             if(i==10){
//                 ESP_LOGE(TAG, "sdcard_scan fail, reboot");
//                 vTaskDelay(1000/portTICK_PERIOD_MS);
//                 esp_restart();
//             }
//             break;
//         }
//     }
//     if(sdcard_list_get_url_num(s_player->sdcard_audio.sdcard_list) == 0){
//         ESP_LOGE(TAG, "[app_player_sdcard_init] No musics scanned");
//         return;
//     }
//     sdcard_list_show(s_player->sdcard_audio.sdcard_list);
//     char *url = NULL;
//     if(ESP_OK != sdcard_list_current(s_player->sdcard_audio.sdcard_list, &url)){
//         ESP_LOGE(TAG,"[app_player_sdcard_init] Music number is null");

//     }
//     else{
//         audio_element_set_uri(s_player->sdcard_audio.element_fatfs, url);
//     }
// }

void app_player_embed_flash_init(audio_event_iface_handle_t evt)
{

    ESP_LOGI(TAG, "[ 1 ] embed flash stream init");
    embed_flash_stream_cfg_t embed_cfg = EMBED_FLASH_STREAM_CFG_DEFAULT();
    embed_cfg.task_core = 1;
    embed_cfg.task_prio = 10;
    s_player->tone_audio.element_flash_stream = embed_flash_stream_init(&embed_cfg);
    // AUDIO_NULL_CHECK(TAG, embed_flash_stream_reader, return);
    embed_flash_stream_set_context(s_player->tone_audio.element_flash_stream, (embed_item_info_t *)&g_embed_tone[0], TONE_URL_MAX);
    audio_element_set_uri(s_player->tone_audio.element_flash_stream, embed_tone_url[TONE_URL_BLOOP_1]);

    ESP_LOGI(TAG, "[2.1] Create mp3 decoder to decode mp3 file and set custom read callback");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    mp3_cfg.task_core = 1;
    mp3_cfg.out_rb_size = RB_SIZE;
    mp3_cfg.task_prio = 10;
    s_player->tone_audio.element_decoder = mp3_decoder_init(&mp3_cfg);
    // AUDIO_NULL_CHECK(TAG, mp3_decoder, return);

    ESP_LOGI(TAG, "[4.1] Create resample element");
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 44100;
    rsp_cfg.src_ch = 2;
    rsp_cfg.dest_rate = SAMPLERATE;
    rsp_cfg.dest_ch = 1;
    rsp_cfg.out_rb_size = RB_SIZE;
    rsp_cfg.task_core = 1;
    rsp_cfg.task_prio = 10;
    s_player->tone_audio.element_rsp_filter = rsp_filter_init(&rsp_cfg);

    ESP_LOGI(TAG, "[4.2] Create raw stream of base mp3 to write data");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = RB_SIZE;
    s_player->tone_audio.element_raw = raw_stream_init(&raw_cfg);

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();

    s_player->tone_audio.pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(s_player->tone_audio.pipeline);
    audio_pipeline_register(s_player->tone_audio.pipeline, s_player->tone_audio.element_flash_stream, "tone_flash");
    audio_pipeline_register(s_player->tone_audio.pipeline, s_player->tone_audio.element_decoder, "tone_decoder");
    audio_pipeline_register(s_player->tone_audio.pipeline, s_player->tone_audio.element_rsp_filter, "tone_filter");
    audio_pipeline_register(s_player->tone_audio.pipeline, s_player->tone_audio.element_raw, "tone_raw");
    const char *link_tag_base[4] = {"tone_flash", "tone_decoder", "tone_filter", "tone_raw"};
    audio_pipeline_link(s_player->tone_audio.pipeline, &link_tag_base[0], 4);

    ringbuf_handle_t rb_tone = audio_element_get_input_ringbuf(s_player->tone_audio.element_raw);
    downmix_set_input_rb(s_player->mixer.element_mixer, rb_tone, 1);
    audio_pipeline_set_listener(s_player->tone_audio.pipeline, s_player->evt);

    ESP_LOGI(TAG, "[ 4 ] Start audio_pipeline");
    // audio_pipeline_run(s_player->tone_audio.pipeline);

    ESP_LOGI(TAG, "app_player_embed_flash_init success");
}
// void app_player_embed_flash_init(){
//     audio_element_handle_t embed_flash_stream_reader
//     embed_flash_stream_cfg_t embed_cfg = EMBED_FLASH_STREAM_CFG_DEFAULT();
//     embed_flash_stream_reader = embed_flash_stream_init(&embed_cfg);
//     AUDIO_NULL_CHECK(TAG, embed_flash_stream_reader, return);
//     embed_flash_stream_set_context(embed_flash_stream_reader, (embed_item_info_t *)&g_embed_tone[0], EMBED_TONE_URL_MAX);

//     // audio_element_set_uri(embed_flash_stream_reader, embed_tone_url[NEW_MESSAGE_MP3]);
// }



void app_palyer_event_handler(void *pv)
{
    app_player_handler_t *player = (app_player_handler_t *)pv;
    while (1)
    {
        /* Handle event interface messages from pipeline
           to set music info and to advance to the next song
        */
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(player->evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[app_palyer_event_handler] Event interface error : %d", ret);
            continue;
        }
        if (msg.source_type == AUDIO_ELEMENT_TYPE_PLAYER)
        {
            if (msg.source == (void *)(player->sdcard_handler))
            {
                if (msg.cmd == SDCARD_PLAYER_EVENT_PLAY_STARTED)
                {
                    ESP_LOGI(TAG, "[app_palyer_event_handler] sdcard event: PLAY_STARTED");
                }
                else if (msg.cmd == SDCARD_PLAYER_EVENT_PLAY_PAUSED)
                {
                    ESP_LOGI(TAG, "[app_palyer_event_handler] sdcard event: PLAY_PAUSED");
                }
                else if (msg.cmd == SDCARD_PLAYER_EVENT_PLAY_RESUMED)
                {
                    ESP_LOGI(TAG, "[app_palyer_event_handler] sdcard event: PLAY_RESUMED");
                }
                else if (msg.cmd == SDCARD_PLAYER_EVENT_PLAY_STOPPED)
                {
                    ESP_LOGI(TAG, "[app_palyer_event_handler] sdcard event: PLAY_STOPPED");
                }
                else if (msg.cmd == SDCARD_PLAYER_EVENT_PLAY_FINISHED)
                {
                    ESP_LOGI(TAG, "[app_palyer_event_handler] sdcard event: PLAY_FINISHED");
                }
            }
            // Handle tone cleanup event (from timer callback)
            else if (msg.source == (void *)s_player && msg.cmd == TONE_PLAYER_CMD_CLEANUP)
            {
                ESP_LOGI(TAG, "[app_palyer_event_handler] Processing tone cleanup");
                // Tone 播放完成，清理 pipeline (可以安全执行阻塞操作)
                downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_SWITCH_OFF);
                downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 0, 1);
                // 先 abort ringbuffer 解除可能阻塞的元素任务
                audio_element_abort_input_ringbuf(s_player->tone_audio.element_decoder);
                audio_element_abort_output_ringbuf(s_player->tone_audio.element_decoder);
                audio_element_abort_input_ringbuf(s_player->tone_audio.element_rsp_filter);
                audio_element_abort_output_ringbuf(s_player->tone_audio.element_rsp_filter);
                audio_element_abort_input_ringbuf(s_player->tone_audio.element_raw);
                audio_pipeline_stop(s_player->tone_audio.pipeline);
                audio_pipeline_wait_for_stop(s_player->tone_audio.pipeline);
                audio_pipeline_terminate(s_player->tone_audio.pipeline);
                audio_pipeline_reset_ringbuffer(s_player->tone_audio.pipeline);
                audio_pipeline_reset_elements(s_player->tone_audio.pipeline);
                audio_pipeline_change_state(s_player->tone_audio.pipeline, AEL_STATE_INIT);
                ESP_LOGW(TAG, "[app_palyer_event_handler] tone audio cleanup finished");
                
                s_player->is_tone_playing = false;
                
                // 检查是否有新的 tone 需要播放
                if (s_player->latest_tone_type != -1) {
                    ESP_LOGI(TAG, "[app_palyer_event_handler] Playing queued tone: %d", s_player->latest_tone_type);
                    _app_player_tone_play_latest();
                }
            }
        }

        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT)
        {
            ESP_LOGI(TAG, "[app_palyer_event_handler] [%s], cmd=%d, data=%d", audio_element_get_tag(msg.source), msg.cmd, (int)msg.data);
            // Music finished, Set music info for a new song to be played
            if (msg.source == (void *)(player->mixer.element_i2s) && msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
            {
                audio_element_status_t el_status = (audio_element_status_t)msg.data;

                ESP_LOGD(TAG, "[app_palyer_event_handler] i2s state: %d", el_status);

                if (el_status == AEL_STATUS_STATE_FINISHED)
                {
                    if (player->current_base_pipeline == player->sdcard_handler &&
                        !app_player_is_sdcard_playing())
                    {
                        ESP_LOGW(TAG, "[app_palyer_event_handler] Ignore mixer finished while sdcard is not playing");
                        FW_ReleaseTimer(player_timer_callback, TIMER_ID_RESET_MIXER_PIPELINE);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "[app_palyer_event_handler]Mixer element_i2s finished");
                    // Use FW_SetTimer: same (handler, timerId) will auto-refresh delay, no duplicate timers
                        FW_SetTimer(player_timer_callback, TIMER_ID_RESET_MIXER_PIPELINE, NULL, 50);  // 50ms delay
                    }
                }
            }
            // if(msg.source == (void *)(player->mixer.element_mixer) && msg.cmd == AEL_MSG_CMD_REPORT_STATUS ){
            //     audio_element_status_t el_status = (audio_element_status_t)msg.data;
            //     if(el_status == AEL_STATUS_STATE_FINISHED){
            //         ESP_LOGW(TAG, "[app_palyer_event_handler]Mixer element_mixer finished");
            //             audio_pipeline_reset_ringbuffer(player->mixer.pipeline);
            //         audio_pipeline_reset_elements(player->mixer.pipeline);
            //         audio_pipeline_change_state(player->mixer.pipeline, AEL_STATE_INIT);
            //         // audio_pipeline_resume(player->mixer.pipeline);
            //         audio_pipeline_run(player->mixer.pipeline);
            //     }
            // }

            // Print music info when receive music info from decoder
            if (msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
            {
                audio_element_info_t music_info = {0};
                audio_element_getinfo(msg.source, &music_info);
                ESP_LOGI(TAG, "[app_palyer_event_handler] Received music info from [%s], sample_rates=%d, bits=%d, ch=%d",
                         audio_element_get_tag(msg.source), music_info.sample_rates, music_info.bits, music_info.channels);
                if (msg.source == (void *)(player->a2dp_audio.element_a2dp_stream))
                {
                    rsp_filter_set_src_info(player->a2dp_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);
                }

                if (msg.source == (void *)player->tone_audio.element_decoder)
                {
                    rsp_filter_set_src_info(player->tone_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);
                }
            }
            /* Stop when the tone pipeline element receives stop event */
            if (msg.source == (void *)player->tone_audio.element_flash_stream && msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
                    // (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED)
                    (intptr_t)msg.data != AEL_STATUS_STATE_RUNNING
                    // || ((int)msg.data == AEL_STATUS_STATE_PAUSED)
                )
            {
                ESP_LOGI(TAG, "[app_palyer_event_handler] New come tone stoped or finsihed");
                player->is_tone_playing = false;
                if (player->latest_tone_type != -1)
                {
                    _app_player_tone_play_latest();
                }
                else{
                    FW_ReleaseTimer(player_timer_callback, TIMER_ID_CHECK_TONE_STATE);
                    downmix_set_work_mode(player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_SWITCH_OFF);
                    downmix_set_input_rb_timeout(player->mixer.element_mixer, 0, 1);
                    audio_pipeline_stop(player->tone_audio.pipeline);
                    audio_pipeline_wait_for_stop(player->tone_audio.pipeline);
                    audio_pipeline_terminate(player->tone_audio.pipeline);
                    // downmix_set_work_mode(player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS);
                    audio_pipeline_reset_ringbuffer(player->tone_audio.pipeline);
                    audio_pipeline_reset_elements(player->tone_audio.pipeline);
                    audio_pipeline_check_items_state(player->tone_audio.pipeline, player->tone_audio.element_flash_stream, AEL_STATE_INIT);
                }
            }

            // Update Play state, set display
            if ((msg.source == (void *)player->a2dp_audio.element_a2dp_stream && player->current_base_pipeline == player->a2dp_audio.pipeline))
            {
                if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
                {
                    audio_element_status_t el_state = (int)msg.data;
                    if (el_state == AEL_STATUS_STATE_RUNNING)
                    {
                        // board_display_set_play_state(MX_PLAY_PLAY);
                    }
                    else
                    {
                        // board_display_set_play_state(MX_PLAY_PAUSE);
                    }
                }
            }
            // Update song number
            // if(msg.source == (void *)player->sdcard_audio.element_fatfs && msg.cmd == AEL_MSG_CMD_REPORT_STATUS){
            //     audio_element_status_t el_state = (int)msg.data;
            //     if(el_state == AEL_STATUS_STATE_RUNNING){
            //         uint16_t num = sdcard_list_get_url_id(s_player->sdcard_audio.sdcard_list);
            //         displayer_track_num(num);
            //     }

            // }
        }
    }
}

static void app_switch2_bt()
{
    if (s_player->current_base_pipeline != s_player->a2dp_audio.pipeline)
    {
        ESP_LOGI(TAG, "[app_switch2_bt] Switch to bluetooth audio");
        if (s_player->sdcard_handler)
        {
            s_player->sdcard_handler->pause();
        }

        if (s_player->a2dp_audio.pipeline == NULL)
        {
            ESP_LOGE(TAG, "[app_switch2_bt] a2dp_audio.pipeline is NULL");
            app_player_a2dp_init();
        }
        s_player->base_stream_read_frozen = false;
        s_player->current_base_pipeline = s_player->a2dp_audio.pipeline;
        s_player->current_base_ringbuf = audio_element_get_input_ringbuf(s_player->a2dp_audio.element_raw);
        _apply_base_stream_input_ringbuf();
        _refresh_base_stream_timeout();
        audio_pipeline_run(s_player->current_base_pipeline);
        audio_pipeline_run(s_player->mixer.pipeline);
        displayer_track_clear();
    }
    else
    {
        ESP_LOGW(TAG, "[app_switch2_bt] already bluetooth audio");
    }
}
void app_switch2_sdcard()
{
    if (s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
    {
        ESP_LOGE(TAG, "[app_switch2_sdcard] A2DP is already connected, cannot switch to sdcard");
        return;
    }
    if (s_player->sdcard_handler == NULL)
    {
        ESP_LOGE(TAG, "[app_switch2_sdcard] sdcard audio pipeline is NULL");
        return;
    }
    if (s_player->current_base_pipeline != s_player->sdcard_handler)
    {
        ESP_LOGI(TAG, "[app_switch2_sdcard] Switch to sdcard audio");
        s_player->current_base_pipeline = s_player->sdcard_handler;
        s_player->current_base_ringbuf = s_player->sdcard_handler->get_ringbuf();
        _apply_base_stream_input_ringbuf();
        _refresh_base_stream_timeout();
        app_player_sdcard_refresh_track_display();
        //     audio_pipeline_run(s_player->current_base_pipeline);
        // audio_pipeline_run(s_player->mixer.pipeline);
    }
    else
    {
        ESP_LOGI(TAG, "[app_switch2_sdcard] already sdcard audio");
    }
}




static void bt_app_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    ESP_LOGI(TAG, "[bt_app_a2dp_cb] event:%d,", event);
    switch (event)
    {
    case ESP_A2D_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "ESP_A2D_CONNECTION_STATE state:%d,bda:" ESP_BD_ADDR_STR "", param->conn_stat.state, ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING)
        {
            ESP_LOGI(TAG, "[bt_app_a2dp_cb] connecting");
        }
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            ESP_LOGI(TAG, "[bt_app_a2dp_cb] connected, bda:" ESP_BD_ADDR_STR "", ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));
            // stop reconnect timer
            app_player_a2dp_auto_reconnect_stop();
            app_bt_non_discoverable();
            memcpy(s_bt_link.remote_bda, param->conn_stat.remote_bda, ESP_BD_ADDR_LEN);
            // board_display_set_bt_state(MX_BT_PAIROK);
            displayer_bt_blink_stop();
            app_player_embed_tone_play(TONE_URL_GREANPATCH);
            s_bt_link.conn_state = param->conn_stat.state;
            app_switch2_bt();
        }
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
        {
            ESP_LOGI(TAG, "[bt_app_a2dp_cb] disconnected");
            if (app_bt_get_connectable())
            {
                displayer_bt_blink_slow();
            }
            else
            {
                displayer_bt_clear();
            }
            s_bt_link.conn_state = param->conn_stat.state;
            app_switch2_sdcard();
            memset(s_bt_link.remote_bda, 0, ESP_BD_ADDR_LEN);

            // // esp_a2d_sink_deinit();
            // esp_avrc_ct_deinit();
            // // esp_avrc_tg_deinit();
            // vTaskDelay(50 / portTICK_PERIOD_MS);
            // // esp_a2d_sink_init();
            // esp_avrc_ct_init();
            // esp_avrc_tg_init();
        }

        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "ESP_A2D_AUDIO_STATE state:%d,bda:" ESP_BD_ADDR_STR "", param->audio_stat.state, ESP_BD_ADDR_HEX(param->audio_stat.remote_bda));
        s_bt_link.audio_state = param->audio_stat.state;
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED)
        {
            _refresh_base_stream_timeout();
            ESP_LOGI(TAG, "[bt_app_a2dp_cb] audio started");
        }
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED)
        {
            ESP_LOGI(TAG, "[bt_app_a2dp_cb] audio stopped");
        }
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND)
        {
            ESP_LOGI(TAG, "[bt_app_a2dp_cb] audio SUSPEND");
        }
        break;
    case ESP_A2D_PROF_STATE_EVT:

        // if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS)
        // {
        //     app_player_a2dp_auto_reconnect_start();
        // }
        break;

    default:
        break;
    }
}

audio_err_t app_player_a2dp_init()
{

    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED)
    {
        ESP_LOGE(TAG, "Bluetooth controller not enabled");
        return ESP_FAIL;
    }
    ESP_LOGW(TAG, "[*] Get Bluetooth stream");
    a2dp_stream_config_t a2dp_config = {
        .type = AUDIO_STREAM_READER,
        // .user_callback = {0},
        .user_callback.user_a2d_cb = bt_app_a2dp_cb,
#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0))
        .audio_hal = s_player->board_handle->audio_hal,
#endif
    };
    s_player->a2dp_audio.element_a2dp_stream = a2dp_stream_init(&a2dp_config);

    ESP_LOGI(TAG, "[4.1] Create resample element");
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 44100;
    rsp_cfg.src_ch = 2;
    rsp_cfg.dest_rate = SAMPLERATE;
    rsp_cfg.dest_ch = 1;
    rsp_cfg.out_rb_size = RB_SIZE;
    s_player->a2dp_audio.element_rsp_filter = rsp_filter_init(&rsp_cfg);

    ESP_LOGI(TAG, "[4.2] Create raw stream of a2dp to write data");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = RB_SIZE;
    s_player->a2dp_audio.element_raw = raw_stream_init(&raw_cfg);

    ESP_LOGI(TAG, "[4.2] Register all elements to audio pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_player->a2dp_audio.pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(s_player->a2dp_audio.pipeline);
    audio_pipeline_register(s_player->a2dp_audio.pipeline, s_player->a2dp_audio.element_a2dp_stream, "a2dp_stream");
    audio_pipeline_register(s_player->a2dp_audio.pipeline, s_player->a2dp_audio.element_rsp_filter, "a2dp_rsp");
    audio_pipeline_register(s_player->a2dp_audio.pipeline, s_player->a2dp_audio.element_raw, "a2dp_raw");
    const char *link_tag_base[3] = {"a2dp_stream", "a2dp_rsp", "a2dp_raw"};
    audio_pipeline_link(s_player->a2dp_audio.pipeline, &link_tag_base[0], 3);

    audio_pipeline_set_listener(s_player->a2dp_audio.pipeline, s_player->evt);
    // s_player->current_base_pipeline = s_player->a2dp_audio.pipeline;

    return ESP_OK;
}

audio_err_t app_player_a2dp_deinit()
{
    ESP_LOGE(TAG, "[*] app_player_a2dp_deinit");
    if (s_player->a2dp_audio.pipeline)
    {
        audio_pipeline_stop(s_player->a2dp_audio.pipeline);
        audio_pipeline_wait_for_stop(s_player->a2dp_audio.pipeline);
        audio_pipeline_terminate(s_player->a2dp_audio.pipeline);
        audio_pipeline_unregister(s_player->a2dp_audio.pipeline, s_player->a2dp_audio.element_a2dp_stream);
        audio_pipeline_unregister(s_player->a2dp_audio.pipeline, s_player->a2dp_audio.element_rsp_filter);
        audio_pipeline_unregister(s_player->a2dp_audio.pipeline, s_player->a2dp_audio.element_raw);

        audio_pipeline_remove_listener(s_player->a2dp_audio.pipeline);

        audio_pipeline_deinit(s_player->a2dp_audio.pipeline);
        audio_element_deinit(s_player->a2dp_audio.element_a2dp_stream);
        audio_element_deinit(s_player->a2dp_audio.element_rsp_filter);
        audio_element_deinit(s_player->a2dp_audio.element_raw);
    }
    a2dp_destroy();

    s_player->a2dp_audio.pipeline = NULL;
    return ESP_OK;
}

// 从NVS读取EQ值到数组
esp_err_t static app_player_load_eq_from_nvs(int *eq_gains)
{
    ESP_LOGI(TAG, "Loading EQ values from NVS");

    if (eq_gains == NULL)
    {
        ESP_LOGE(TAG, "Invalid eq_gains pointer");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    // 打开NVS
    err = nvs_open(NVS_EQ_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // 读取EQ值到传入的数组
    int eq_gains_tmp[EQ_BANDS_NUM];
    size_t required_size = EQ_BANDS_NUM * sizeof(int);
    err = nvs_get_blob(nvs_handle, NVS_EQ_KEY, eq_gains_tmp, &required_size);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "Successfully loaded EQ values from NVS");
        for (int i = 0; i < EQ_BANDS_NUM; i++)
        {
            ESP_LOGI(TAG, "Band %d: %d dB", i, eq_gains_tmp[i]);
            eq_gains[i] = eq_gains_tmp[i];
        }
    }
    else if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        ESP_LOGW(TAG, "EQ values not found in NVS");
    }
    else
    {
        ESP_LOGE(TAG, "Error reading from NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}

// 保存EQ值到NVS
esp_err_t static app_player_save_eq_to_nvs(const int *eq_gains)
{

    if (eq_gains == NULL)
    {
        ESP_LOGE(TAG, "Invalid EQ gains pointer");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    // 打开NVS
    err = nvs_open(NVS_EQ_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // 写入EQ值
    err = nvs_set_blob(nvs_handle, NVS_EQ_KEY, eq_gains, EQ_BANDS_NUM * sizeof(int));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error writing to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // 提交更改
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    }
    else
    {
        ESP_LOGI(TAG, "Successfully saved EQ values to NVS");
    }

    nvs_close(nvs_handle);
    return err;
}

const int *app_player_get_eq()
{
    return s_player->mixer.eq_gains;
}

esp_err_t app_player_set_eq(int *eq_gains)
{
    ESP_LOGI(TAG, "app_player_set_eq");

    for (int i = 0; i < EQ_BANDS_NUM; i++)
    {

        // s_player->mixer.eq_gains[i] = eq_gains[i];
        equalizer_set_gain_info(s_player->mixer.element_equalizer, i, eq_gains[i], true);
    }
    app_player_save_eq_to_nvs(eq_gains);
    return ESP_OK;
}

bool app_player_is_a2dp_connected()
{
    if (s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
    {
        return true;
    }
    return false;
}

esp_err_t app_player_a2dp_disconnect()
{
    if (memcmp(s_bt_link.remote_bda, "\0\0\0\0\0\0", ESP_BD_ADDR_LEN) != 0)
    {
        return esp_a2d_sink_disconnect(s_bt_link.remote_bda);
    }
    return ESP_OK;
}

static esp_err_t _app_player_prepare_base_for_shutdown_tone(void)
{
    if (s_player == NULL || s_player->mixer.element_mixer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    FW_ReleaseTimer(player_timer_callback, TIMER_ID_RESET_MIXER_PIPELINE);
    FW_ReleaseTimer(player_timer_callback, TIMER_ID_RECONNECT);
    s_player->latest_tone_type = -1;

    if (s_player->current_base_pipeline == s_player->sdcard_handler &&
        s_player->sdcard_handler != NULL) {
        /* Freeze SD output at an empty ringbuffer so tone can take over cleanly. */
        app_player_set_base_stream_silence_wait(true);
        app_player_set_base_stream_read_freeze(true);
        s_player->sdcard_handler->pause();
    } else if (s_player->current_base_pipeline == s_player->a2dp_audio.pipeline &&
               s_player->a2dp_audio.pipeline != NULL) {
        audio_pipeline_stop(s_player->a2dp_audio.pipeline);
        audio_pipeline_wait_for_stop(s_player->a2dp_audio.pipeline);
    }

    downmix_set_input_rb_timeout(s_player->mixer.element_mixer,
                                 SHUTDOWN_TONE_BASE_TIMEOUT_MS,
                                 MIX_INDEX_BASE_STREAM);

    if (s_player->mixer.pipeline != NULL &&
        audio_pipeline_get_state(s_player->mixer.pipeline) != AEL_STATE_RUNNING) {
        audio_pipeline_reset_ringbuffer(s_player->mixer.pipeline);
        audio_pipeline_reset_elements(s_player->mixer.pipeline);
        audio_pipeline_change_state(s_player->mixer.pipeline, AEL_STATE_INIT);
        audio_pipeline_run(s_player->mixer.pipeline);
    }
    return ESP_OK;
}

static esp_err_t _app_player_force_tone_play(uint8_t tone_type)
{
    if (tone_type >= TONE_URL_MAX || s_player == NULL ||
        s_player->tone_audio.element_raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FW_ReleaseTimer(player_timer_callback, TIMER_ID_CHECK_TONE_STATE);
    s_player->latest_tone_type = tone_type;
    _app_player_tone_play_latest();
    return ESP_OK;
}

esp_err_t app_player_play_shutdown_tone(uint8_t tone_type, uint32_t timeout_ms)
{
    uint32_t waited_ms = 0;
    esp_err_t ret;

    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = _app_player_prepare_base_for_shutdown_tone();
    if (ret != ESP_OK) {
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_TONE_SETTLE_MS));

    ret = _app_player_force_tone_play(tone_type);
    if (ret != ESP_OK) {
        return ret;
    }

    while (s_player->is_tone_playing && waited_ms < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(SHUTDOWN_TONE_POLL_MS));
        waited_ms += SHUTDOWN_TONE_POLL_MS;
    }

    if (s_player->is_tone_playing) {
        ESP_LOGW(TAG, "[app_player_play_shutdown_tone] timeout, force continue shutdown");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

esp_err_t app_player_prepare_shutdown(void)
{
    if (s_player == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    FW_ReleaseTimer(player_timer_callback, TIMER_ID_CHECK_TONE_STATE);
    FW_ReleaseTimer(player_timer_callback, TIMER_ID_RESET_MIXER_PIPELINE);
    FW_ReleaseTimer(player_timer_callback, TIMER_ID_RECONNECT);

    s_player->latest_tone_type = -1;
    s_player->is_tone_playing = false;

    if (s_player->tone_audio.element_decoder != NULL) {
        audio_element_abort_input_ringbuf(s_player->tone_audio.element_decoder);
        audio_element_abort_output_ringbuf(s_player->tone_audio.element_decoder);
    }
    if (s_player->tone_audio.element_rsp_filter != NULL) {
        audio_element_abort_input_ringbuf(s_player->tone_audio.element_rsp_filter);
        audio_element_abort_output_ringbuf(s_player->tone_audio.element_rsp_filter);
    }
    if (s_player->tone_audio.element_raw != NULL) {
        audio_element_abort_input_ringbuf(s_player->tone_audio.element_raw);
    }
    if (s_player->tone_audio.pipeline != NULL) {
        audio_pipeline_stop(s_player->tone_audio.pipeline);
        audio_pipeline_wait_for_stop(s_player->tone_audio.pipeline);
    }

    if (s_player->a2dp_audio.pipeline != NULL) {
        audio_pipeline_stop(s_player->a2dp_audio.pipeline);
        audio_pipeline_wait_for_stop(s_player->a2dp_audio.pipeline);
    }

    if (s_player->mixer.pipeline != NULL) {
        audio_pipeline_stop(s_player->mixer.pipeline);
        audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
    }

    if (s_player->board_handle != NULL && s_player->board_handle->audio_hal != NULL) {
        audio_hal_ctrl_codec(s_player->board_handle->audio_hal,
                             AUDIO_HAL_CODEC_MODE_DECODE,
                             AUDIO_HAL_CTRL_STOP);
    }

    board_amplifier_disable();
    return ESP_OK;
}

void app_player_a2dp_auto_reconnect_start()
{
    // 设置1秒后启动重连定时器
    if (FW_SetTimer(player_timer_callback, TIMER_ID_RECONNECT, NULL, 1000) != pdPASS)
    {
        ESP_LOGE(TAG, "[app_player_a2dp_auto_reconnect_start] FW_SetTimer failed");
    }
    else
    {
        ESP_LOGI(TAG, "[app_player_a2dp_auto_reconnect_start] success");
    }
}

void app_player_a2dp_auto_reconnect_stop()
{
    FW_ReleaseTimer(player_timer_callback, TIMER_ID_RECONNECT);
    ESP_LOGI(TAG, "[app_player_a2dp_auto_reconnect_stop] success");
}

audio_err_t app_player_init(audio_board_handle_t board_handle)
{
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set("AUDIO_ELEMENT", ESP_LOG_DEBUG);
    esp_log_level_set("AUDIO_PIPELINE", ESP_LOG_VERBOSE);
    esp_log_level_set("EQUALIZER", ESP_LOG_DEBUG);
    esp_log_level_set("A2DP_STREAM", ESP_LOG_INFO);
    // esp_log_level_set("BT_KEYCTRL", ESP_LOG_DEBUG);

    audio_err_t ret = ESP_OK;
    // init the audio board and codec
    s_player = audio_calloc(1, sizeof(app_player_handler_t));
    AUDIO_MEM_CHECK(TAG, s_player, return ESP_ERR_AUDIO_MEMORY_LACK);
    memset(s_player, 0, sizeof(app_player_handler_t));
    s_player->latest_tone_type = -1;
    s_player->is_tone_playing = false;
    s_player->current_base_ringbuf = NULL;
    s_player->base_stream_read_frozen = false;
    s_player->frozen_base_ringbuf = rb_create(BASE_STREAM_FREEZE_RB_BLOCK_SIZE,
                                              BASE_STREAM_FREEZE_RB_BLOCK_COUNT);
    AUDIO_MEM_CHECK(TAG, s_player->frozen_base_ringbuf, return ESP_ERR_AUDIO_MEMORY_LACK);
    ESP_LOGI(TAG, "[3.0] Create pipeline_mix pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_player->mixer.pipeline = audio_pipeline_init(&pipeline_cfg);

    if (board_handle)
    {
        s_player->board_handle = board_handle;
    }
    ESP_LOGI(TAG, "[3.1] Create down-mixer element");
    downmix_cfg_t downmix_cfg = DEFAULT_DOWNMIX_CONFIG();
    // downmix_cfg.task_stack = 10*1024;
    downmix_cfg.out_rb_size = RB_SIZE;
    downmix_cfg.max_sample = 4 * 1024;
    downmix_cfg.task_core = 1;
    downmix_cfg.stack_in_ext = 1;
    downmix_cfg.task_prio = 12;
    downmix_cfg.downmix_info.source_num = MIX_INDEX_NUMBER_SOURCE_FILE;
    s_player->mixer.element_mixer = downmix_init(&downmix_cfg);
    downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 0, MIX_INDEX_BASE_STREAM);
    downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 0, MIX_INDEX_TONE_STREAM);

    esp_downmix_input_info_t source_information[MIX_INDEX_NUMBER_SOURCE_FILE] = {0};
    esp_downmix_input_info_t source_info_base = {
        .samplerate = SAMPLERATE,
        .channel = NUM_INPUT_CHANNEL,
        .bits_num = 16,
        /* base music depress form 0dB to -10dB */
        .gain = {-7, -15},
        .transit_time = TRANSMITTIME,
    };
    source_information[0] = source_info_base;

    esp_downmix_input_info_t source_info_newcome = {
        .samplerate = SAMPLERATE,
        .channel = NUM_INPUT_CHANNEL,
        .bits_num = 16,
        /* newcome music rise form -10dB to 0dB */
        .gain = {-15, -7},
        .transit_time = TRANSMITTIME,
    };
    source_information[1] = source_info_newcome;
    source_info_init(s_player->mixer.element_mixer, source_information);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, SAMPLERATE, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    i2s_cfg.out_rb_size = 32 * 1024; // Increase I2S buffer size to prevent underrun at high bitrates
    // i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, SAMPLERATE, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    s_player->mixer.element_i2s = i2s_stream_init(&i2s_cfg);
    i2s_stream_set_clk(s_player->mixer.element_i2s, SAMPLERATE, 16, ESP_DOWNMIX_OUTPUT_TYPE_ONE_CHANNEL);

    // Equalizer
    app_player_load_eq_from_nvs(s_player->mixer.eq_gains);
    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    eq_cfg.channel = 1;
    eq_cfg.samplerate = SAMPLERATE;
    eq_cfg.set_gain = s_player->mixer.eq_gains;
    eq_cfg.stack_in_ext = 1;
    eq_cfg.out_rb_size = RB_SIZE;
    s_player->mixer.element_equalizer = equalizer_init(&eq_cfg);

    ESP_LOGI(TAG, "[3.3] Register elements player->mixer.element_mixer i2s_writer");
    audio_pipeline_register(s_player->mixer.pipeline, s_player->mixer.element_mixer, "mixer");
    audio_pipeline_register(s_player->mixer.pipeline, s_player->mixer.element_equalizer, "equalizer");
    audio_pipeline_register(s_player->mixer.pipeline, s_player->mixer.element_i2s, "i2s");

    ESP_LOGI(TAG, "[3.4] Link elements together player->mixer.element_mixer-->i2s_stream-->[codec_chip]");
    const char *link_mix[3] = {"mixer", "equalizer", "i2s"};
    audio_pipeline_link(s_player->mixer.pipeline, &link_mix[0], 3);

    ESP_LOGI(TAG, "[5.0] Set up  event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    /* 主事件口同时承接 mixer、tone、a2dp、sdcard 转发事件，适当放大队列降低突发丢事件概率。 */
    evt_cfg.internal_queue_size = 80;
    evt_cfg.external_queue_size = 80;
    evt_cfg.queue_set_size = 80;
    s_player->evt = audio_event_iface_init(&evt_cfg);

    audio_pipeline_set_listener(s_player->mixer.pipeline, s_player->evt);
    downmix_set_output_type(s_player->mixer.element_mixer, ESP_DOWNMIX_OUTPUT_TYPE_ONE_CHANNEL);
    i2s_stream_set_clk(s_player->mixer.element_i2s, SAMPLERATE, 16, ESP_DOWNMIX_OUTPUT_TYPE_ONE_CHANNEL);

    // audio_pipeline_run(player->mixer.element_mixer);

    // audio_pipeline_run(player->sdcard_audio.pipeline);
    // app_player_sdcard_init(s_player->evt);

    audio_pipeline_run(s_player->mixer.pipeline);

    downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS);

    _refresh_base_stream_timeout();
    audio_board_enable_sdcard_reader();
    // vTaskDelay(5000 / portTICK_PERIOD_MS);
    app_player_a2dp_init();

    app_player_embed_flash_init(s_player->evt);

    // ret = xTaskCreatePinnedToCore(app_palyer_event_handler, "palyer_event_handler",
    //                           s_player, 10 * 1024, 20, NULL, ESP_TASK_MAIN_CORE);

    ret = audio_thread_create(&s_player->tsk_handle, "palyer_event_handler1", app_palyer_event_handler,
                              s_player, 3 * 1024, 13, true, 1);
    // ret = xTaskCreatePinnedToCore(app_palyer_event_handler, "palyer_event_handler", (10 * 1024), NULL, 15, NULL, ESP_TASK_MAIN_CORE);
    if (ret == ESP_FAIL)
    {
        ESP_LOGE(TAG, "Create audio manager task failure");
        ret = ESP_ERR_AUDIO_MEMORY_LACK;
        // goto ep_init_err;
    }
    // set default player.
    // s_player->current_base_pipeline = s_player->sdcard_audio.pipeline;
    // audio_pipeline_run(player->sdcard_audio.pipeline);

    ESP_LOGD(TAG, "player->evt:%d", (int)s_player->evt);
    ESP_LOGD(TAG, "player->mixer.pipeline:%d,mixer:%d,i2s:%d", (int)s_player->mixer.pipeline, (int)s_player->mixer.element_mixer, (int)s_player->mixer.element_i2s);

    ESP_LOGD(TAG, "player->tone_audio.pipeline:%d", (int)s_player->tone_audio.pipeline);
    ESP_LOGD(TAG, "player->tone_audio.element_flash_stream:%d", (int)s_player->tone_audio.element_flash_stream);
    ESP_LOGD(TAG, "player->tone_audio.element_decoder:%d", (int)s_player->tone_audio.element_decoder);
    ESP_LOGD(TAG, "player->tone_audio.element_rsp_filter:%d", (int)s_player->tone_audio.element_rsp_filter);
    ESP_LOGD(TAG, "player->tone_audio.element_raw:%d", (int)s_player->tone_audio.element_raw);

    return ret;
}

// void app_player_sdcard_play(const char *uri)
// {
//     ESP_LOGI(TAG, " Play music: %s", uri);
//     if (s_player->current_base_pipeline != NULL)
//     {
//         audio_pipeline_pause(s_player->current_base_pipeline);
//     }
//     if (s_player->current_base_pipeline != s_player->sdcard_audio.pipeline)
//     {
//         ringbuf_handle_t rb_base = audio_element_get_input_ringbuf(s_player->sdcard_audio.element_raw);
//         ESP_LOGI(TAG, "app_player_sdcard_play set sdcard as base ");
//         downmix_set_input_rb(s_player->mixer.element_mixer, rb_base, 0);
//     }
//     char *url1 = NULL;
//     sdcard_list_current(s_player->sdcard_audio.sdcard_list, &url1);
//     // audio_element_set_uri(player->sdcard_audio.element_fatfs, url);
//     if (ESP_OK != audio_element_set_uri(s_player->sdcard_audio.element_fatfs, url1))
//     {
//         ESP_LOGI(TAG, "audio_element_set_uri not ok");
//     }
//     ESP_LOGI(TAG, " Play music: %s", url1);
//     s_player->current_base_pipeline = s_player->sdcard_audio.pipeline;
//     // vTaskDelay(10/portTICK_PERIOD_MS);
//     audio_pipeline_run(s_player->current_base_pipeline);
//     // audio_pipeline_run(player->mixer.pipeline);

//     // downmix_set_work_mode(player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS);

//     downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
// }



static void _app_player_tone_play_latest()
{
    int type = s_player->latest_tone_type;
    if (type >= TONE_URL_MAX || type == -1)
    {
        s_player->is_tone_playing = false;
        return;
    }
    
    ESP_LOGI(TAG, "[_app_player_tone_play] Tone music start run, num: %d, url: %s", type, embed_tone_url[type]);
    
    // 先清除 latest_tone_type，避免播放完成后重复播放
    s_player->latest_tone_type = -1;
    s_player->is_tone_playing = true;
    
    // 应用音频控制原则：先 abort ringbuffer 解除可能阻塞的元素任务
    audio_element_abort_input_ringbuf(s_player->tone_audio.element_decoder);
    audio_element_abort_output_ringbuf(s_player->tone_audio.element_decoder);
    audio_element_abort_input_ringbuf(s_player->tone_audio.element_rsp_filter);
    audio_element_abort_output_ringbuf(s_player->tone_audio.element_rsp_filter);
    audio_element_abort_input_ringbuf(s_player->tone_audio.element_raw);
    
    // 终止并重置 pipeline
    audio_pipeline_terminate(s_player->tone_audio.pipeline);
    audio_pipeline_reset_ringbuffer(s_player->tone_audio.pipeline);
    audio_pipeline_reset_elements(s_player->tone_audio.pipeline);
    audio_pipeline_change_state(s_player->tone_audio.pipeline, AEL_STATE_INIT);
    
    // 设置新的 URI 并启动
    audio_element_set_uri(s_player->tone_audio.element_flash_stream, embed_tone_url[type]);
    audio_pipeline_run(s_player->tone_audio.pipeline);
    
    // 配置 downmix
    downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_SWITCH_ON);
    downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, MIX_INDEX_TONE_STREAM);
    
    ESP_LOGI(TAG, "[_app_player_tone_play] Tone music running...");
    
    // 启动 timer 检查播放状态
    FW_SetTimer(player_timer_callback, TIMER_ID_CHECK_TONE_STATE, 0, 500);
}

void app_player_embed_tone_play(uint8_t tone_type)
{
    // return;
    if (tone_type >= TONE_URL_MAX || s_player->tone_audio.element_raw == NULL)
    {

        return;
    }
    
    ESP_LOGI(TAG, "[app_player_embed_tone_play] Request tone: %d", tone_type);
    s_player->latest_tone_type = tone_type;

    if (s_player->is_tone_playing) {
        ESP_LOGI(TAG, "[app_player_embed_tone_play] Tone is playing, update latest only");
        FW_SetTimer(player_timer_callback, TIMER_ID_CHECK_TONE_STATE, 0, 200);
        return;
    }
    _app_player_tone_play_latest();

    // audio_event_iface_msg_t msg = {0};
    // msg.source_type = AUDIO_ELEMENT_TYPE_PLAYER;
    // msg.source = NULL;
    // msg.cmd = TONE_PLAYER_CMD_PLAY;
    // msg.data = NULL;
    // esp_err_t ret = audio_event_iface_sendout(s_player->evt, &msg);
    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "[app_player_embed_tone_play] Failed to send event: %d", ret);
    // }
    ESP_LOGI(TAG, "[app_player_embed_tone_play] Tone music running...");
}

typedef enum
{
    PLAY_ACITN_NONE = 0,
    PLAY_ACITN_PLAY,
    PLAY_ACITN_PAUSE,
} player_action_t;
uint8_t last_action = PLAY_ACITN_NONE;

// void player_check_mixer_is_normal(){
//     if((audio_element_get_state(s_player->mixer.element_i2s) != AEL_STATE_RUNNING)
//         || (audio_element_get_state(s_player->mixer.element_mixer) != AEL_STATE_RUNNING)
//         || (audio_element_get_state(s_player->mixer.element_equalizer) != AEL_STATE_RUNNING)){
//         ESP_LOGE(TAG, "[player_check_mixer_is_normal] mixer i2s not running");
//         // audio_pipeline_stop(s_player->mixer.pipeline);
//         // audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
//         // audio_pipeline_terminate(s_player->mixer.pipeline);

//         audio_pipeline_reset_ringbuffer(s_player->mixer.pipeline);
//         audio_pipeline_reset_elements(s_player->mixer.pipeline);
//         audio_pipeline_change_state(s_player->mixer.pipeline,AEL_STATE_INIT);
//         audio_pipeline_run(s_player->mixer.pipeline);
//     }
// }

void app_player_on_play()
{
    // check mixer pipeline

    // no player source
    if (s_player->current_base_pipeline == NULL)
    {
        ESP_LOGW(TAG, "[app_player_on_play] current_base_pipeline is null ");
        return;
    }
    // when plying bt audio but disconnected
    if (s_player->current_base_pipeline != s_player->sdcard_handler && /*esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED*/ s_bt_link.conn_state != ESP_A2D_CONNECTION_STATE_CONNECTED)
    {
        ESP_LOGE(TAG, "[app_player_on_play] Bluetooth controller not enabled");
        app_switch2_sdcard();
    }
    if (s_player->current_base_pipeline == s_player->a2dp_audio.pipeline)
    {
        if (s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            if (s_bt_link.audio_state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND || s_bt_link.audio_state == ESP_A2D_AUDIO_STATE_STOPPED || last_action == PLAY_ACITN_PAUSE)
            {

                ESP_LOGI(TAG, "[app_player_on_play] A2DP not started, start it");
                periph_bt_play(app_bt_get_periph());
                last_action = PLAY_ACITN_PLAY;
            }
            else
            {

                ESP_LOGI(TAG, "[app_player_on_play] A2DP started, pause it");
                periph_bt_pause(app_bt_get_periph());
                last_action = PLAY_ACITN_PAUSE;
            }
        }
        else
        {
            ESP_LOGE(TAG, "[app_player_on_play] A2DP not connected ");
        }
        return;
    }
    // when playing sdcard music
    else if (s_player->current_base_pipeline == s_player->sdcard_handler)
    {
        s_player->sdcard_handler->play_pause();
        FW_ReleaseTimer(player_timer_callback, TIMER_ID_RESET_MIXER_PIPELINE);
        _refresh_base_stream_timeout();
    }
    else
    {
        ESP_LOGE(TAG, "[app_player_on_play] Not support audio source ");
    }
    last_action = PLAY_ACITN_NONE;
}

void app_player_on_next()
{

    ESP_LOGD(TAG, "[app_player_on_next] ");
    // no player source
    if (s_player->current_base_pipeline == NULL)
    {
        ESP_LOGW(TAG, "[app_player_on_next] current_base_pipeline is null ");
        return;
    }
    // Bluetooth
    if (s_player->current_base_pipeline == s_player->a2dp_audio.pipeline)
    {
        if (s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            ESP_LOGI(TAG, "[app_player_on_next] A2DP next song");
            periph_bt_avrc_next(app_bt_get_periph());
        }
        else
        {
            ESP_LOGE(TAG, "[app_player_on_next] A2DP not connected ");
        }
    }
    // SDcard
    else if (s_player->current_base_pipeline == s_player->sdcard_handler)
    {
        s_player->sdcard_handler->next();
        FW_ReleaseTimer(player_timer_callback, TIMER_ID_RESET_MIXER_PIPELINE);
        _refresh_base_stream_timeout();
    }
    else
    {
        ESP_LOGE(TAG, "[app_player_on_next] app_player_on_next: Not support audio source ");
    }
}
void app_player_on_prev()
{
    ESP_LOGD(TAG, "[app_player_on_prev] ");
    // no player source
    if (s_player->current_base_pipeline == NULL)
    {
        ESP_LOGW(TAG, "[app_player_on_prev] current_base_pipeline is null ");
        return;
    }
    // Bluetooth
    if (s_player->current_base_pipeline == s_player->a2dp_audio.pipeline)
    {
        if (s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
        {
            ESP_LOGI(TAG, "[app_player_on_prev] A2DP previous song");
            periph_bt_avrc_prev(app_bt_get_periph());
        }
        else
        {
            ESP_LOGE(TAG, "[*] A2DP not connected");
        }
    }
    // SD card
    else if (s_player->current_base_pipeline == s_player->sdcard_handler)
    {
        s_player->sdcard_handler->prev();
    }
    else
    {
        ESP_LOGE(TAG, "[*] app_player_on_prev: Not support audio source");
    }
}

void app_player_on_volume_up()
{
    if (s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
    {
        periph_bt_volume_up(app_bt_get_periph());
        ESP_LOGI(TAG, "[ * ] Remote volume+");
    }
    else
    {
        int player_volume;
        if (app_player_get_user_volume(&player_volume) != ESP_OK) {
            audio_hal_get_volume(s_player->board_handle->audio_hal, &player_volume);
        }
        player_volume += 10;
        if (player_volume > 100)
        {
            player_volume = 100;
        }
        audio_hal_set_volume(s_player->board_handle->audio_hal, player_volume);
        ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
    }
}
void app_player_on_volume_down()
{
    if (s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED)
    {
        periph_bt_volume_down(app_bt_get_periph());
        ESP_LOGI(TAG, "[ * ] Remote volume-");
    }
    else
    {
        int player_volume;
        if (app_player_get_user_volume(&player_volume) != ESP_OK) {
            audio_hal_get_volume(s_player->board_handle->audio_hal, &player_volume);
        }
        player_volume -= 10;
        if (player_volume < 0)
        {
            player_volume = 0;
        }
        audio_hal_set_volume(s_player->board_handle->audio_hal, player_volume);
        ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
    }
}

void audio_player_show_element_status()
{
    ESP_LOGD(TAG, "\r\n-----------Element status:--------------- \r\n");
    if (s_player)
    {
        ESP_LOGD(TAG, "current_base_pipeline:[%4x]", (unsigned int)s_player->current_base_pipeline);
        // 检查mixer pipeline及其元素是否有效
        if (s_player->mixer.pipeline)
        {
            ESP_LOGD(TAG, "mixer[%4x]pipeline:%d,mixer:%d,eq:%d i2s:%d",
                     (unsigned int)s_player->mixer.pipeline,
                     s_player->mixer.pipeline ? audio_pipeline_get_state(s_player->mixer.pipeline) : -1,
                     s_player->mixer.element_mixer ? audio_element_get_state(s_player->mixer.element_mixer) : -1,
                     s_player->mixer.element_equalizer ? audio_element_get_state(s_player->mixer.element_equalizer) : -1,
                     s_player->mixer.element_i2s ? audio_element_get_state(s_player->mixer.element_i2s) : -1);
        }

        // 检查sdcard pipeline及其元素是否有效
        // if(s_player->sdcard_audio.pipeline) {
        //     ESP_LOGD(TAG, "sdcard[%4x].pipeline:%d,fatfs:%d,decoder:%d,rsp_filter:%d,raw:%d",
        //         (unsigned int)s_player->sdcard_audio.pipeline,
        //         s_player->sdcard_audio.pipeline ? audio_pipeline_get_state(s_player->sdcard_audio.pipeline) : -1,
        //         s_player->sdcard_audio.element_fatfs ? audio_element_get_state(s_player->sdcard_audio.element_fatfs) : -1,
        //         s_player->sdcard_audio.element_decoder ? audio_element_get_state(s_player->sdcard_audio.element_decoder) : -1,
        //         s_player->sdcard_audio.element_rsp_filter ? audio_element_get_state(s_player->sdcard_audio.element_rsp_filter) : -1,
        //         s_player->sdcard_audio.element_raw ? audio_element_get_state(s_player->sdcard_audio.element_raw) : -1);
        // }

        // 检查a2dp pipeline及其元素是否有效
        if (s_player->a2dp_audio.pipeline)
        {
            ESP_LOGD(TAG, "a2dp[%4x].pipeline:%d,a2dp_stream:%d,rsp_filter:%d,raw:%d",
                     (unsigned int)s_player->a2dp_audio.pipeline,
                     s_player->a2dp_audio.pipeline ? audio_pipeline_get_state(s_player->a2dp_audio.pipeline) : -1,
                     s_player->a2dp_audio.element_a2dp_stream ? audio_element_get_state(s_player->a2dp_audio.element_a2dp_stream) : -1,
                     s_player->a2dp_audio.element_rsp_filter ? audio_element_get_state(s_player->a2dp_audio.element_rsp_filter) : -1,
                     s_player->a2dp_audio.element_raw ? audio_element_get_state(s_player->a2dp_audio.element_raw) : -1);
        }
        app_player_sdcard_print_state();
    }
}

esp_err_t app_player_sdcard_on_unmount()
{
    if (s_player->sdcard_handler)
    {
        s_player->sdcard_handler->stop();

        // esp_err_t ret;
        // if(s_player->current_base_pipeline == s_player->sdcard_handler){
        //     s_player->current_base_pipeline = NULL;
        // }
        // // audio_pipeline_stop(s_player->sdcard_audio.pipeline);
        // // audio_pipeline_wait_for_stop(s_player->sdcard_audio.pipeline);
        // audio_pipeline_terminate(s_player->sdcard_audio.pipeline);
        // audio_pipeline_reset_ringbuffer(s_player->sdcard_audio.pipeline);
        // audio_pipeline_reset_elements(s_player->sdcard_audio.pipeline);
        // audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_INIT);
        // return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_player_on_mount()
{

    app_player_sdcard_init(s_player->evt);
    s_player->sdcard_handler = app_player_sdcard_get_interface();
    if (s_player->sdcard_handler == NULL)
    {
        ESP_LOGE(TAG, "[app_player_on_mount] sdcard_handler is null");
        return ESP_ERR_NOT_SUPPORTED;
    }
    app_switch2_sdcard();
    return ESP_OK;
}

bool app_player_is_sdcard_playing()
{
    if (s_player && s_player->sdcard_handler && s_player->sdcard_handler->is_playing)
    {
        return s_player->sdcard_handler->is_playing();
    }
    return false;
}
