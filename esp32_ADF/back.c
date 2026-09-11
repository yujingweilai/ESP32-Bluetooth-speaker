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
#include "periph_sdcard.h"
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

#include "sdcard_list.h"
#include "sdcard_scan.h"
#include "esp_decoder.h"
#include "app_bt.h"
#include "board.h"

#include "equalizer.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/semphr.h"
#include "esp_bt.h"

#include "esp_timer.h"
#include <inttypes.h>

static const char *TAG = "APP_PlAYER";
#define MIX_INDEX_NUMBER_SOURCE_FILE 2
#define MIX_INDEX_BASE_STREAM 0
#define MIX_INDEX_TONE_STREAM 1
// #define SAMPLERATE 44100
#define SAMPLERATE 48000
#define NUM_INPUT_CHANNEL 1
#define TRANSMITTIME 500
// #define TRANSMITTIME 3000

#define NVS_EQ_NAMESPACE "eq_config"
#define NVS_EQ_KEY "eq_gains"
#define EQ_BANDS_NUM 10 // 均衡器频段数量

typedef struct
{
    audio_pipeline_handle_t     pipeline;
    audio_element_handle_t      element_mixer;
    audio_element_handle_t      element_equalizer;
    audio_element_handle_t      element_i2s;
    int eq_gains[EQ_BANDS_NUM];
} mixer_handler_t;

typedef struct
{
    audio_pipeline_handle_t     pipeline;
    audio_element_handle_t      element_fatfs;
    audio_element_handle_t      element_decoder;
    audio_element_handle_t      element_rsp_filter;
    audio_element_handle_t      element_raw;
    playlist_operator_handle_t  sdcard_list;
    uint8_t                     operator_lock;
} sdcard_audio_handler_t;

typedef struct{
    audio_pipeline_handle_t     pipeline;
    audio_element_handle_t      element_a2dp_stream;
    audio_element_handle_t      element_rsp_filter;
    audio_element_handle_t      element_raw;

}a2dp_audio_handler_t;
typedef struct
{
    audio_pipeline_handle_t     pipeline;
    audio_element_handle_t      element_flash_stream;
    audio_element_handle_t      element_decoder;
    audio_element_handle_t      element_rsp_filter;
    audio_element_handle_t      element_raw;
} embed_audio_handler_t;
typedef struct 
{
    esp_a2d_connection_state_t conn_state;
    esp_a2d_audio_state_t audio_state;
    esp_bd_addr_t remote_bda;
}app_bt_link_t;
static app_bt_link_t s_bt_link;

typedef struct
{
    audio_event_iface_handle_t  evt;
    audio_thread_t              tsk_handle;
    audio_thread_t              sdcard_handler;
    mixer_handler_t             mixer;
    sdcard_audio_handler_t      sdcard_audio;
    embed_audio_handler_t       tone_audio;
    a2dp_audio_handler_t        a2dp_audio;
    audio_pipeline_handle_t     current_base_pipeline;
    audio_board_handle_t        board_handle;
    esp_timer_handle_t          state_check_timer;
    bool                        block_pause;    
} app_player_handler_t;

static app_player_handler_t *s_player;


/* static function declare*/
/*------------------------------*/

void play_state_check_timer_callback(void* arg)
{
    audio_element_state_t state = (audio_element_state_t)(intptr_t)arg;
    ESP_LOGI(TAG, "[play_state_check_timer]timer_callback, id%d", state);
    if(s_player->current_base_pipeline == s_player->sdcard_audio.pipeline){
        // if(ESP_OK != audio_pipeline_check_items_state(s_player->sdcard_audio.pipeline, s_player->sdcard_audio.element_fatfs, state))
        // {
        //     ESP_LOGE(TAG, "state no aligned");

                
        if(state == AEL_STATE_RUNNING){
            if(audio_pipeline_get_state(s_player->sdcard_audio.pipeline) != state){
                ESP_LOGW(TAG, "[play_state_check_timer]sdcard_audio pipeline state no aligned");
                audio_pipeline_stop(s_player->sdcard_audio.pipeline);
                audio_pipeline_wait_for_stop(s_player->sdcard_audio.pipeline);

                audio_pipeline_terminate(s_player->sdcard_audio.pipeline);
                audio_pipeline_change_state(s_player->sdcard_audio.pipeline, AEL_STATE_INIT);
                audio_pipeline_reset_ringbuffer(s_player->sdcard_audio.pipeline);
                audio_pipeline_reset_elements(s_player->sdcard_audio.pipeline);
                audio_pipeline_run(s_player->sdcard_audio.pipeline);
            }
            else
            {
                bool all_running = true;
                if(audio_element_get_state(s_player->sdcard_audio.element_fatfs) != state){
                    ESP_LOGW(TAG, "[play_state_check_timer]fatfs state no aligned");
                    // audio_element_change_state(s_player->sdcard_audio.element_fatfs, state);
                    // audio_element_run(s_player->sdcard_audio.element_fatfs);
                    all_running = false;
                }
                if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_fatfs)){
                    ESP_LOGW(TAG, "[play_state_check_timer]fatfs not running");
                    // audio_element_resume(s_player->sdcard_audio.element_fatfs, 0 , 200);
                    all_running = false;
                }
                if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_decoder)){
                    ESP_LOGW(TAG, "[play_state_check_timer]decoder not running");
                    // audio_element_resume(s_player->sdcard_audio.element_decoder, 0 , 200);
                    all_running = false;
                }
                if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_rsp_filter)){
                    ESP_LOGW(TAG, "[play_state_check_timer]rsp_filter not running");
                    // audio_element_resume(s_player->sdcard_audio.element_rsp_filter, 0 , 200);
                    all_running = false;
                }
                if(AEL_STATE_RUNNING != audio_element_get_state(s_player->sdcard_audio.element_raw)){
                    ESP_LOGW(TAG, "[play_state_check_timer]raw not running");
                    // audio_element_resume(s_player->sdcard_audio.element_raw, 0 , 200);
                    all_running = false;
                }
                if( !all_running ){
                    ESP_LOGW(TAG, "[play_state_check_timer]some element not running");
                    audio_pipeline_stop(s_player->current_base_pipeline);
                    audio_pipeline_wait_for_stop(s_player->current_base_pipeline);
                    audio_pipeline_terminate(s_player->current_base_pipeline);
 
                    audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_INIT);
                    audio_pipeline_reset_ringbuffer(s_player->current_base_pipeline);
                    audio_pipeline_reset_elements(s_player->current_base_pipeline);
                    // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
                    audio_pipeline_run(s_player->current_base_pipeline);
                }
            }
            //check mixer state
            if( audio_pipeline_get_state(s_player->mixer.pipeline) != state ){
                audio_pipeline_stop(s_player->mixer.pipeline);
                audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
                audio_pipeline_terminate(s_player->mixer.pipeline);
                audio_pipeline_change_state(s_player->mixer.pipeline, AEL_STATE_INIT);
                audio_pipeline_run(s_player->mixer.pipeline);
            }
            else{
                if(AEL_STATE_RUNNING != audio_element_get_state(s_player->mixer.element_mixer) || AEL_STATE_RUNNING != audio_element_get_state(s_player->mixer.element_equalizer) || AEL_STATE_RUNNING != audio_element_get_state(s_player->mixer.element_i2s)){
                    audio_pipeline_stop(s_player->mixer.pipeline);
                    audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
                    audio_pipeline_terminate(s_player->mixer.pipeline);
                    audio_pipeline_change_state(s_player->mixer.pipeline, AEL_STATE_INIT);
                    audio_pipeline_run(s_player->mixer.pipeline);
                }
            }
        }
    }
    if(s_player->block_pause) s_player->block_pause = false;
    
    s_player->sdcard_audio.operator_lock = false;
}



void app_palyer_event_sdcard_handler(void *pv)
{
    audio_event_iface_handle_t *evt = (audio_event_iface_handle_t *)pv;
    while (1)
    {
        /* Handle event interface messages from pipeline
           to set music info and to advance to the next song
        */
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[app_palyer_event_sdcard_handler] Event interface error : %d", ret);
            continue;
        }
        if(s_player->current_base_pipeline!= s_player->sdcard_audio.pipeline){
            continue;
        }
        ESP_LOGI(TAG, "[app_palyer_event_sdcard_handler] [%s], cmd=%d, data=%d", audio_element_get_tag(msg.source), msg.cmd, (int)msg.data );
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT)
        {
            // Advance to the next song when previous finishes
            if (msg.source == (void *)(s_player->sdcard_audio.element_rsp_filter) && msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
            {
                audio_element_state_t el_state = audio_element_get_state(s_player->sdcard_audio.element_rsp_filter);
                ESP_LOGD(TAG,"[app_palyer_event_sdcard_handler] rsp_filte state: %d", el_state);
                // if(s_player->current_base_pipeline!= s_player->sdcard_audio.pipeline){
                //     continue;
                // }
                // else 
                if (el_state == AEL_STATE_FINISHED)
                {
                    ESP_LOGI(TAG, "[app_palyer_event_sdcard_handler] Music Finished");
                    if (s_player->current_base_pipeline == s_player->sdcard_audio.pipeline)
                    {
                        ESP_LOGI(TAG, "[app_palyer_event_sdcard_handler] SDcard music next");
                        char *url = NULL;
                        sdcard_list_next(s_player->sdcard_audio.sdcard_list, 1, &url);
                        ESP_LOGI(TAG, "[app_palyer_event_sdcard_handler] URL: %s", url);
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
                ESP_LOGI(TAG, "[app_palyer_event_sdcard_handler] Received music info from [%s], sample_rates=%d, bits=%d, ch=%d",
                            audio_element_get_tag(msg.source),music_info.sample_rates, music_info.bits, music_info.channels);                            

                if (msg.source == (void *)s_player->sdcard_audio.element_decoder){
                    rsp_filter_set_src_info(s_player->sdcard_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);
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
            //     ESP_LOGI(TAG, "New come music stoped or finsihed");
            // }

            // Update Play state, set display
            if((msg.source == (void *)s_player->sdcard_audio.element_fatfs && s_player->current_base_pipeline == s_player->sdcard_audio.pipeline)){
                if(msg.cmd == AEL_MSG_CMD_REPORT_STATUS ){
                    audio_element_status_t el_state = (int)msg.data;
                    if(el_state == AEL_STATUS_STATE_RUNNING){
                        // board_display_set_play_state(MX_PLAY_PLAY);
                    }
                    else{
                        // board_display_set_play_state(MX_PLAY_PAUSE);
                    }
                }
            }
            // Update song number
            if(msg.source == (void *)s_player->sdcard_audio.element_fatfs && msg.cmd == AEL_MSG_CMD_REPORT_STATUS){
                audio_element_status_t el_state = (int)msg.data;
                if(el_state == AEL_STATUS_STATE_RUNNING){
                    uint16_t num = sdcard_list_get_url_id(s_player->sdcard_audio.sdcard_list);
                    board_display_set_song_num(num);
                }
            
            }
            //audio element err
            if(msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (int)(msg.data) < AEL_STATUS_ERROR_UNKNOWN){
                ESP_LOGE(TAG, "[app_palyer_event_sdcard_handler] audio element err");
                if(msg.source == s_player->sdcard_audio.element_decoder){
                    ESP_LOGE(TAG, "[app_palyer_event_sdcard_handler] sdcard_audio.element_decoder err,  %d", (int)msg.data);
                    app_player_on_next();       
                }
                if(msg.source == s_player->sdcard_audio.element_fatfs){
                    ESP_LOGE(TAG, "[app_palyer_event_sdcard_handler] sdcard_audio.element_fatfs err,  %d", (int)msg.data);
                    app_player_on_next();  
                }
                if(msg.source == s_player->sdcard_audio.element_raw){
                    ESP_LOGE(TAG, "[app_palyer_event_sdcard_handler] sdcard_audio.element_raw err, %d", (int)msg.data);
                    app_player_on_next();  
                }
                if(msg.source == s_player->sdcard_audio.element_rsp_filter){
                    ESP_LOGE(TAG, "[app_palyer_event_sdcard_handler] sdcard_audio.element_rsp_filter err, %d", (int)msg.data); 
                    app_player_on_next();  
                }
            }
        }
    }
}

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
            //     ESP_LOGI(TAG, "New come music stoped or finsihed");
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
                    board_display_set_song_num(num);
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
            audio_pipeline_stop(s_player->tone_audio.pipeline);
            audio_pipeline_wait_for_stop(s_player->tone_audio.pipeline);
            audio_pipeline_terminate(s_player->mixer.pipeline);
            audio_pipeline_reset_ringbuffer(s_player->tone_audio.pipeline);
            audio_pipeline_reset_elements(s_player->tone_audio.pipeline);
            // audio_pipeline_check_items_state(s_player->tone_audio.pipeline, s_player->tone_audio.element_flash_stream,AEL_STATE_INIT);
            downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS); //will lead to alwas in bypass mode
            ESP_LOGI(TAG, "[app_palyer_event_handler] Tone music stoped or finsihed2");
        }
    }
}
#endif  



void set_play_state_check_timer(audio_element_state_t state){
    
    s_player->sdcard_audio.operator_lock = true;
    if(s_player->state_check_timer != NULL){
        esp_timer_stop(s_player->state_check_timer);
        esp_timer_delete(s_player->state_check_timer);
    }
    esp_timer_create_args_t timer_args = {
        .callback = play_state_check_timer_callback,
        .arg = (void*)(intptr_t)state,
        .name = "state_check_timer",
        .dispatch_method = ESP_TIMER_TASK
    };
    esp_timer_create(&timer_args, &s_player->state_check_timer);
    esp_timer_start_once(s_player->state_check_timer, 1500 * 1000);
    
}


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


void sdcard_url_save_cb(void *user_data, char *url)
{
    playlist_operator_handle_t sdcard_handle = (playlist_operator_handle_t)user_data;
    esp_err_t ret = sdcard_list_save(sdcard_handle, url);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Fail to save sdcard url to sdcard playlist");
    }
}



void app_sdcard_scan(){

    ESP_LOGI(TAG, "[1.2] Set up a sdcard playlist and scan sdcard music save to it");
    
    if(s_player->sdcard_audio.sdcard_list != NULL){
        sdcard_list_destroy(s_player->sdcard_audio.sdcard_list);
    }
    if(ESP_OK != sdcard_list_create(&(s_player->sdcard_audio.sdcard_list))){
        ESP_LOGE(TAG,"[app_player_sdcard_init] Create sdcard_list fail");
        return ;
    }

    for(uint8_t i=0;i<10;i++){
        if(ESP_OK != sdcard_scan(sdcard_url_save_cb, "/sdcard", 0, (const char *[]) {"mp3", "m4a", "flac", "ogg", "opus", "amr", "ts", "aac", "wav"}, 9, s_player->sdcard_audio.sdcard_list)){
            ESP_LOGW(TAG, "sdcard scan fail retry..");
            vTaskDelay(500/portTICK_PERIOD_MS);
        }else{
            if(i==10){
                ESP_LOGE(TAG, "sdcard_scan fail, reboot");
                vTaskDelay(1000/portTICK_PERIOD_MS);
                esp_restart();
            }
            break;
        }
    }
    if(sdcard_list_get_url_num(s_player->sdcard_audio.sdcard_list) == 0){
        ESP_LOGE(TAG, "[app_player_sdcard_init] No musics scanned");
        return;
    }
    sdcard_list_show(s_player->sdcard_audio.sdcard_list);
    char *url = NULL;
    if(ESP_OK != sdcard_list_current(s_player->sdcard_audio.sdcard_list, &url)){
        ESP_LOGE(TAG,"[app_player_sdcard_init] Music number is null");
        
    }
    else{
        audio_element_set_uri(s_player->sdcard_audio.element_fatfs, url);
    }
}


esp_err_t app_player_sdcard_init( )
{


    ESP_LOGI(TAG, "[4.0] Create Fatfs stream to read input data");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.ext_stack = true;
    fatfs_cfg.type = AUDIO_STREAM_READER;
    s_player->sdcard_audio.element_fatfs = fatfs_stream_init(&fatfs_cfg);
    if(s_player->sdcard_audio.element_fatfs == NULL){
        ESP_LOGE(TAG,"[app_player_sdcard_init] fatfs init fail");
        return ESP_FAIL;
    }


    app_sdcard_scan();

    // audio_element_set_uri(player->sdcard_audio.element_fatfs, "/sdcard/music.mp3");

    //-------
    // audio_element_set_uri(player->sdcard_audio.element_fatfs, "/sdcard/music.mp3");
    // player->sdcard_audio.element_fatfs = fatfs_stream_init(&fatfs_cfg);
    // audio_element_set_uri(player->sdcard_audio.element_fatfs, "/sdcard/tone.mp3");
    //------------

    ESP_LOGI(TAG, "[4.1] Create mp3 decoder to decode mp3 file");
    // Add esp_decoder
    audio_decoder_t auto_decode[] = {
        DEFAULT_ESP_AMRNB_DECODER_CONFIG(),
        DEFAULT_ESP_AMRWB_DECODER_CONFIG(),
        DEFAULT_ESP_MP3_DECODER_CONFIG(),
        DEFAULT_ESP_WAV_DECODER_CONFIG(),
        DEFAULT_ESP_AAC_DECODER_CONFIG(),
        DEFAULT_ESP_M4A_DECODER_CONFIG(),
        DEFAULT_ESP_TS_DECODER_CONFIG(),
        DEFAULT_ESP_FLAC_DECODER_CONFIG(),
        DEFAULT_ESP_PCM_DECODER_CONFIG(),
    };
    esp_decoder_cfg_t auto_dec_cfg = DEFAULT_ESP_DECODER_CONFIG();
    auto_dec_cfg.task_prio = 12;
    s_player->sdcard_audio.element_decoder = esp_decoder_init(&auto_dec_cfg, auto_decode, sizeof(auto_decode) / sizeof(audio_decoder_t));
    if(s_player->sdcard_audio.element_decoder == NULL){
        ESP_LOGE(TAG,"[app_player_sdcard_init] element_decoder init fail");
        return ESP_FAIL;
    }


    ESP_LOGI(TAG, "[4.1] Create resample element");
    rsp_filter_cfg_t rsp_sdcard_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_sdcard_cfg.src_rate = 44100;
    rsp_sdcard_cfg.src_ch = 2;
    rsp_sdcard_cfg.dest_rate = SAMPLERATE;
    rsp_sdcard_cfg.dest_ch = 1;
    s_player->sdcard_audio.element_rsp_filter = rsp_filter_init(&rsp_sdcard_cfg);
    if(s_player->sdcard_audio.element_rsp_filter == NULL){
        ESP_LOGE(TAG,"[app_player_sdcard_init] element_rsp_filter init fail");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[4.2] Create raw stream of base mp3 to write data");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    s_player->sdcard_audio.element_raw = raw_stream_init(&raw_cfg);
    if(s_player->sdcard_audio.element_raw == NULL){
        ESP_LOGE(TAG,"[app_player_sdcard_init] element_raw init fail");
        return ESP_FAIL;       
    }

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_player->sdcard_audio.pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(s_player->sdcard_audio.pipeline);
    audio_pipeline_register(s_player->sdcard_audio.pipeline, s_player->sdcard_audio.element_fatfs, "base_fatfs");
    audio_pipeline_register(s_player->sdcard_audio.pipeline, s_player->sdcard_audio.element_decoder, "base_decoder");
    audio_pipeline_register(s_player->sdcard_audio.pipeline, s_player->sdcard_audio.element_rsp_filter, "base_filter");
    audio_pipeline_register(s_player->sdcard_audio.pipeline, s_player->sdcard_audio.element_raw, "base_raw");
    const char *link_tag_base[4] = {"base_fatfs", "base_decoder", "base_filter", "base_raw"};
    audio_pipeline_link(s_player->sdcard_audio.pipeline, &link_tag_base[0], 4);

    ringbuf_handle_t rb_base = audio_element_get_input_ringbuf(s_player->sdcard_audio.element_raw);
    downmix_set_input_rb(s_player->mixer.element_mixer, rb_base, 0);

    ESP_LOGI(TAG, "[5.0] Set up  sdcard event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    evt_cfg.internal_queue_size = 40;
    evt_cfg.external_queue_size = 40;
    audio_event_iface_handle_t sdcard_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_player->sdcard_audio.pipeline, sdcard_evt);


    esp_err_t ret = audio_thread_create(&s_player->sdcard_handler, "palyer_event_handler2", app_palyer_event_sdcard_handler,
                              sdcard_evt, 3 * 1024, 13, true, 1);
    // ret = xTaskCreatePinnedToCore(app_palyer_event_handler, "palyer_event_handler", (10 * 1024), NULL, 15, NULL, ESP_TASK_MAIN_CORE);   
    if (ret == ESP_FAIL)
    {
        ESP_LOGE(TAG, "Create audio manager task failure");
        ret = ESP_ERR_AUDIO_MEMORY_LACK;
        // goto ep_init_err;
    }


    ESP_LOGI(TAG, "app_player_sdcard_init success");
    if(s_player->current_base_pipeline == NULL){
        app_switch2_sdcard();
        s_player->current_base_pipeline = s_player->sdcard_audio.pipeline;
        ESP_LOGI(TAG,"Set current_base_pipeline to sdcard_audio.pipeline");
    }
    ESP_LOGW(TAG, "player->sdcard_audio.pipeline:%d,fatfs:%d,decoder:%d,rsp_filter:%d,raw:%d,sdcard_list:%d",(int)s_player->sdcard_audio.pipeline,
    (int)s_player->sdcard_audio.element_fatfs,    (int)s_player->sdcard_audio.element_decoder,
    (int)s_player->sdcard_audio.element_rsp_filter,    (int)s_player->sdcard_audio.element_raw,(int)s_player->sdcard_audio.sdcard_list );
    return ESP_OK;
}

esp_err_t app_player_sdcard_init_needed(){
    if(s_player->sdcard_audio.element_fatfs == NULL || s_player->sdcard_audio.pipeline == NULL){
       return app_player_sdcard_init();
    }
    else{
        app_sdcard_scan();
    }
    return ESP_OK;
}
void app_player_embed_flash_init(audio_event_iface_handle_t evt)
{

    ESP_LOGI(TAG, "[ 1 ] embed flash stream init");
    embed_flash_stream_cfg_t embed_cfg = EMBED_FLASH_STREAM_CFG_DEFAULT();
    s_player->tone_audio.element_flash_stream = embed_flash_stream_init(&embed_cfg);
    // AUDIO_NULL_CHECK(TAG, embed_flash_stream_reader, return);
    embed_flash_stream_set_context(s_player->tone_audio.element_flash_stream, (embed_item_info_t *)&g_embed_tone[0], TONE_URL_MAX);
    audio_element_set_uri(s_player->tone_audio.element_flash_stream, embed_tone_url[TONE_URL_BLOOP_1]);

    ESP_LOGI(TAG, "[2.1] Create mp3 decoder to decode mp3 file and set custom read callback");
    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
    s_player->tone_audio.element_decoder = mp3_decoder_init(&mp3_cfg);
    // AUDIO_NULL_CHECK(TAG, mp3_decoder, return);

    ESP_LOGI(TAG, "[4.1] Create resample element");
    rsp_filter_cfg_t rsp_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_cfg.src_rate = 44100;
    rsp_cfg.src_ch = 2;
    rsp_cfg.dest_rate = SAMPLERATE;
    rsp_cfg.dest_ch = 1;
    s_player->tone_audio.element_rsp_filter = rsp_filter_init(&rsp_cfg);

    ESP_LOGI(TAG, "[4.2] Create raw stream of base mp3 to write data");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
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
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT)
        {
            ESP_LOGI(TAG, "[app_palyer_event_handler] [%s], cmd=%d, data=%d", audio_element_get_tag(msg.source), msg.cmd, (int)msg.data );
            // Music finished, Set music info for a new song to be played
            if(msg.source == (void *)(player->mixer.element_i2s) && msg.cmd == AEL_MSG_CMD_REPORT_STATUS ){
                audio_element_status_t el_status = (audio_element_status_t)msg.data;

                ESP_LOGD(TAG,"[app_palyer_event_handler] i2s state: %d", el_status);
                // if(player->current_base_pipeline != player->sdcard_audio.pipeline){
                    

                // }
                // else 
                if(el_status == AEL_STATUS_STATE_FINISHED){
                    ESP_LOGW(TAG, "[app_palyer_event_handler]Mixer element_i2s finished");
                    audio_pipeline_reset_ringbuffer(player->mixer.pipeline);
                    audio_pipeline_reset_elements(player->mixer.pipeline);
                    audio_pipeline_change_state(player->mixer.pipeline, AEL_STATE_INIT);
                    // audio_pipeline_resume(player->mixer.pipeline);
                    audio_pipeline_run(player->mixer.pipeline);
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

            // Advance to the next song when previous finishes
            // if (msg.source == (void *)(player->sdcard_audio.element_rsp_filter) && msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
            // {
            //     audio_element_state_t el_state = audio_element_get_state(player->sdcard_audio.element_rsp_filter);
            //     ESP_LOGD(TAG,"[ * ] rsp_filte state: %d", el_state);
            //     if(player->current_base_pipeline!= player->sdcard_audio.pipeline){
                    
            //     }
            //     else if (el_state == AEL_STATE_FINISHED)
            //     {
            //         ESP_LOGI(TAG, "[app_palyer_event_handler] Music Finished");
            //         if (player->current_base_pipeline == player->sdcard_audio.pipeline)
            //         {
            //             ESP_LOGI(TAG, "[app_palyer_event_handler] SDcard music next");
            //             char *url = NULL;
            //             sdcard_list_next(player->sdcard_audio.sdcard_list, 1, &url);
            //             ESP_LOGI(TAG, "[app_palyer_event_handler] URL: %s", url);
            //             /* In previous versions, audio_pipeline_terminal() was called here. It will close all the element task and when we use
            //              * the pipeline next time, all the tasks should be restarted again. It wastes too much time when we switch to another music.
            //              * So we use another method to achieve this as below.
            //              */
            //             audio_element_set_uri(player->sdcard_audio.element_fatfs, url);
            //             audio_pipeline_reset_ringbuffer(player->sdcard_audio.pipeline);
            //             audio_pipeline_reset_elements(player->sdcard_audio.pipeline);
            //             audio_pipeline_change_state(player->sdcard_audio.pipeline, AEL_STATE_INIT);
            //             // audio_pipeline_resume(player->mixer.pipeline);
            //             audio_pipeline_run(player->sdcard_audio.pipeline);
            //         }
            //     }
            // }
            // Print music info when receive music info from decoder
            if(msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO){
                audio_element_info_t music_info = {0};
                audio_element_getinfo(msg.source, &music_info);
                ESP_LOGI(TAG, "[app_palyer_event_handler] Received music info from [%s], sample_rates=%d, bits=%d, ch=%d",
                            audio_element_get_tag(msg.source),music_info.sample_rates, music_info.bits, music_info.channels);                            
                if(msg.source == (void *)(player->a2dp_audio.element_a2dp_stream) ){
                    rsp_filter_set_src_info(player->a2dp_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);                
                }
                if (msg.source == (void *)player->sdcard_audio.element_decoder){
                    rsp_filter_set_src_info(player->sdcard_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);
                }
                if (msg.source == (void *)player->tone_audio.element_decoder){
                    rsp_filter_set_src_info(player->tone_audio.element_rsp_filter, music_info.sample_rates, music_info.channels);
                }
            }
            /* Stop when the tone pipeline element receives stop event */
            if ( msg.source == (void *)player->tone_audio.element_rsp_filter
                && msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (((int)msg.data == AEL_STATUS_STATE_STOPPED)
                        || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
                downmix_set_work_mode(player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_SWITCH_OFF);
                downmix_set_input_rb_timeout(player->mixer.element_mixer, 1, 1);
                audio_pipeline_stop(player->tone_audio.pipeline);
                audio_pipeline_wait_for_stop(player->tone_audio.pipeline);
                // audio_pipeline_terminate(player->mixer.pipeline);
                downmix_set_work_mode(player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS);
                audio_pipeline_reset_ringbuffer(player->tone_audio.pipeline);
                audio_pipeline_reset_elements(player->tone_audio.pipeline);
                audio_pipeline_check_items_state(player->tone_audio.pipeline, player->tone_audio.element_flash_stream,AEL_STATE_INIT);
                ESP_LOGI(TAG, "[app_palyer_event_handler] New come music stoped or finsihed");
            }

            // Update Play state, set display
            if((msg.source == (void *)player->a2dp_audio.element_a2dp_stream && player->current_base_pipeline == player->a2dp_audio.pipeline) 
                || (msg.source == (void *)player->sdcard_audio.element_fatfs && player->current_base_pipeline == player->sdcard_audio.pipeline)){
                if(msg.cmd == AEL_MSG_CMD_REPORT_STATUS ){
                    audio_element_status_t el_state = (int)msg.data;
                    if(el_state == AEL_STATUS_STATE_RUNNING){
                        // board_display_set_play_state(MX_PLAY_PLAY);
                    }
                    else{
                        // board_display_set_play_state(MX_PLAY_PAUSE);
                    }
                }
            }
            // Update song number
            // if(msg.source == (void *)player->sdcard_audio.element_fatfs && msg.cmd == AEL_MSG_CMD_REPORT_STATUS){
            //     audio_element_status_t el_state = (int)msg.data;
            //     if(el_state == AEL_STATUS_STATE_RUNNING){
            //         uint16_t num = sdcard_list_get_url_id(s_player->sdcard_audio.sdcard_list);
            //         board_display_set_song_num(num);
            //     }
            
            // }
   
        }
    }
}

static void app_switch2_bt(){
    if(s_player->current_base_pipeline != s_player->a2dp_audio.pipeline){
        ESP_LOGI(TAG,"[app_switch2_bt] Switch to bluetooth audio");
        if(audio_pipeline_get_state(s_player->current_base_pipeline) == AEL_STATE_RUNNING){
            ESP_LOGI(TAG,"[app_switch2_bt] Pause current audio");
            if(audio_element_pause(s_player->sdcard_audio.element_fatfs)!= ESP_OK){
                audio_pipeline_pause(s_player->sdcard_audio.pipeline);
            }    
            audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_PAUSED);
        }
        
        // audio_pipeline_stop(s_player->mixer.pipeline);
        // audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
        // audio_pipeline_terminate(s_player->mixer.pipeline);
        // audio_pipeline_change_state(s_player->mixer.pipeline,AEL_STATE_INIT);
        // audio_pipeline_reset_ringbuffer(s_player->mixer.pipeline);
        // audio_pipeline_reset_elements(s_player->mixer.pipeline);
        // audio_pipeline_run(s_player->mixer.pipeline);

        // audio_pipeline_stop(s_player->a2dp_audio.pipeline);
        // audio_pipeline_wait_for_stop(s_player->a2dp_audio.pipeline);
        // audio_pipeline_terminate(s_player->a2dp_audio.pipeline);
        // audio_pipeline_change_state(s_player->a2dp_audio.pipeline,AEL_STATE_INIT);
        // audio_pipeline_reset_ringbuffer(s_player->a2dp_audio.pipeline);
        // audio_pipeline_reset_elements(s_player->a2dp_audio.pipeline);
        // // audio_pipeline_run(s_player->a2dp_audio.pipeline);


        if(s_player->a2dp_audio.pipeline == NULL){
            ESP_LOGE(TAG,"[app_switch2_bt] a2dp_audio.pipeline is NULL");
            app_player_a2dp_init();
        }
        s_player->current_base_pipeline = s_player->a2dp_audio.pipeline;
        ringbuf_handle_t rb_base = audio_element_get_input_ringbuf(s_player->a2dp_audio.element_raw);
        downmix_set_input_rb(s_player->mixer.element_mixer, rb_base, 0);
        downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
        audio_pipeline_run(s_player->current_base_pipeline);
        audio_pipeline_run(s_player->mixer.pipeline);

    }
    else{
        ESP_LOGW(TAG,"[app_switch2_bt] already bluetooth audio");
    }
    
}
void app_switch2_sdcard(){
    if(s_player->sdcard_audio.pipeline == NULL || s_player->sdcard_audio.element_fatfs == NULL){
        ESP_LOGE(TAG,"[*] sdcard audio pipeline is NULL");
        return;
    }
    if(s_player->current_base_pipeline != s_player->sdcard_audio.pipeline){
        ESP_LOGI(TAG,"[*] Switch to sdcard audio");
        s_player->current_base_pipeline = s_player->sdcard_audio.pipeline;
        
        ringbuf_handle_t rb_base = audio_element_get_input_ringbuf(s_player->sdcard_audio.element_raw);
        downmix_set_input_rb(s_player->mixer.element_mixer, rb_base, 0);
        // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
        //     audio_pipeline_run(s_player->current_base_pipeline);
        // audio_pipeline_run(s_player->mixer.pipeline);
    }
    else{
        ESP_LOGI(TAG,"[*] already sdcard audio");
    }
    
    
}

static esp_err_t reconnect_timer = NULL;
static void reconnect_timer_callback(void* arg)
{
    ESP_LOGI(TAG,"[reconnect_timer_callback] reconnect");
    if(esp_bt_gap_get_bond_device_num() > 0){        
        if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {  
            ESP_LOGE(TAG, "Bluetooth stack is not enabled");
        }
        else{
            esp_bd_addr_t *bdaddr = app_bt_get_last_connected_bdaddr();
            ESP_LOGD(TAG, "-------Last connected device address: "ESP_BD_ADDR_STR"---------" ,
                    ESP_BD_ADDR_HEX(bdaddr[0]));
            ESP_LOGI(TAG,"[reconnect_timer_callback] reconnect");
            esp_a2d_sink_connect(bdaddr[0]);
        }
    }
    else{
        ESP_LOGI(TAG,"[reconnect_timer_callback] no bond device");
    }
}

static void bt_app_a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    ESP_LOGI(TAG,"[bt_app_a2dp_cb] event:%d,",event);
    switch( event ){
        case ESP_A2D_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG,"ESP_A2D_CONNECTION_STATE state:%d,bda:" ESP_BD_ADDR_STR "",param->conn_stat.state, ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));
            if(param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING){
                ESP_LOGI(TAG,"[bt_app_a2dp_cb] connecting");
                
            }
            if(param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED){
                ESP_LOGI(TAG,"[bt_app_a2dp_cb] connected");
                // stop reconnect timer
                esp_timer_stop(reconnect_timer);
                app_bt_non_discoverable();
                memcpy(s_bt_link.remote_bda,param->conn_stat.remote_bda, ESP_BD_ADDR_LEN );
                app_switch2_bt();            
                // board_display_set_bt_state(MX_BT_PAIROK);
                app_player_embed_tone_play(TONE_URL_GREANPATCH);
                s_bt_link.conn_state = param->conn_stat.state;
            }
            if(param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED){
                ESP_LOGI(TAG,"[bt_app_a2dp_cb] disconnected");
                app_switch2_sdcard();
                if(app_bt_get_connectable()){
                    // board_display_set_bt_state(MX_BT_Reconnect); 
                }
                else{
                    // board_display_set_bt_state(MX_BT_Reconnect);
                }
                s_bt_link.conn_state = param->conn_stat.state;
                memset(s_bt_link.remote_bda,0, ESP_BD_ADDR_LEN);
            }
              
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            ESP_LOGI(TAG,"ESP_A2D_AUDIO_STATE state:%d,bda:" ESP_BD_ADDR_STR "",param->audio_stat.state, ESP_BD_ADDR_HEX(param->audio_stat.remote_bda));
            s_bt_link.audio_state = param->audio_stat.state;
            if(param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED){
                downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
                ESP_LOGI(TAG,"[bt_app_a2dp_cb] audio started");
            }
            if(param->audio_stat.state == ESP_A2D_AUDIO_STATE_STOPPED){
                ESP_LOGI(TAG,"[bt_app_a2dp_cb] audio stopped");
            }
            if(param->audio_stat.state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND){
                ESP_LOGI(TAG,"[bt_app_a2dp_cb] audio SUSPEND");
            }
            break;
        case ESP_A2D_PROF_STATE_EVT:

            
            if (param->a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS) {
                // a timer to reconnect
                esp_timer_create_args_t timer_args = {
                    .callback = reconnect_timer_callback,
                    .name = "reconnect_timer"
                };
                if(esp_timer_create(&timer_args, &reconnect_timer) == ESP_OK){
                    esp_timer_start_once(reconnect_timer, 1000*1000);
                    ESP_LOGI(TAG,"[bt_app_a2dp_cb] a2d_prof_stat.init_state == ESP_A2D_INIT_SUCCESS");
                }
                else{
                    ESP_LOGE(TAG,"[bt_app_a2dp_cb] esp_timer_create failed");
                }


            }
            break;
    
        default:
            break;
    }
}

audio_err_t app_player_a2dp_init(){

    if(esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED){
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
    s_player->a2dp_audio.element_rsp_filter = rsp_filter_init(&rsp_cfg);


    ESP_LOGI(TAG, "[4.2] Create raw stream of a2dp to write data");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
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

audio_err_t app_player_a2dp_deinit(){
    ESP_LOGE(TAG, "[*] app_player_a2dp_deinit");
    if(s_player->a2dp_audio.pipeline){
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
esp_err_t static app_player_load_eq_from_nvs(int *eq_gains) {
    ESP_LOGI(TAG, "Loading EQ values from NVS");
    
    if (eq_gains == NULL) {
        ESP_LOGE(TAG, "Invalid eq_gains pointer");
        return ESP_ERR_INVALID_ARG;
    }
    
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    // 打开NVS
    err = nvs_open(NVS_EQ_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // 读取EQ值到传入的数组
    int eq_gains_tmp[EQ_BANDS_NUM];
    size_t required_size = EQ_BANDS_NUM * sizeof(int);
    err = nvs_get_blob(nvs_handle, NVS_EQ_KEY, eq_gains_tmp, &required_size);
    

    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Successfully loaded EQ values from NVS");
        for (int i = 0; i < EQ_BANDS_NUM; i++) {
            ESP_LOGI(TAG, "Band %d: %d dB", i, eq_gains_tmp[i]);
            eq_gains[i] = eq_gains_tmp[i];
        }
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "EQ values not found in NVS");
    } else {
        ESP_LOGE(TAG, "Error reading from NVS: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
    return err;
}

// 保存EQ值到NVS
esp_err_t static app_player_save_eq_to_nvs(const int *eq_gains) {
    
    if (eq_gains == NULL) {
        ESP_LOGE(TAG, "Invalid EQ gains pointer");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    // 打开NVS
    err = nvs_open(NVS_EQ_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        return err;
    }

    // 写入EQ值
    err = nvs_set_blob(nvs_handle, NVS_EQ_KEY, eq_gains, EQ_BANDS_NUM * sizeof(int));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error writing to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return err;
    }

    // 提交更改
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Successfully saved EQ values to NVS");
    }

    nvs_close(nvs_handle);
    return err;
}

const int *app_player_get_eq(){
    return s_player->mixer.eq_gains;
}

esp_err_t app_player_set_eq( int *eq_gains ){
    ESP_LOGI(TAG, "app_player_set_eq");
    
    for(int i = 0; i < EQ_BANDS_NUM; i++){

        // s_player->mixer.eq_gains[i] = eq_gains[i];        
        equalizer_set_gain_info(s_player->mixer.element_equalizer, i, eq_gains[i] , true);
    }
    app_player_save_eq_to_nvs(eq_gains);
    return ESP_OK;
}

esp_err_t app_player_a2dp_disconnect()
{
    return esp_a2d_sink_disconnect(s_bt_link.remote_bda);
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
    ESP_LOGI(TAG, "[3.0] Create pipeline_mix pipeline");
    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_player->mixer.pipeline = audio_pipeline_init(&pipeline_cfg);

    if(board_handle){
        s_player->board_handle = board_handle;
    }
    ESP_LOGI(TAG, "[3.1] Create down-mixer element");
    downmix_cfg_t downmix_cfg = DEFAULT_DOWNMIX_CONFIG();
    downmix_cfg.task_stack = 10*1024;
    downmix_cfg.out_rb_size = 8*1024;
    downmix_cfg.max_sample = 2*1024;
    downmix_cfg.task_core = 1;
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
        .gain = {-7, -9},
        .transit_time = TRANSMITTIME,
    };
    source_information[0] = source_info_base;

    esp_downmix_input_info_t source_info_newcome = {
        .samplerate = SAMPLERATE,
        .channel = NUM_INPUT_CHANNEL,
        .bits_num = 16,
        /* newcome music rise form -10dB to 0dB */
        .gain = {-9, -7},
        .transit_time = TRANSMITTIME,
    };
    source_information[1] = source_info_newcome;
    source_info_init(s_player->mixer.element_mixer, source_information);

    ESP_LOGI(TAG, "[3.2] Create i2s stream to read audio data from codec chip");
    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, SAMPLERATE, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    //i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT_WITH_PARA(I2S_NUM_0, SAMPLERATE, I2S_DATA_BIT_WIDTH_16BIT, AUDIO_STREAM_WRITER);
    s_player->mixer.element_i2s = i2s_stream_init(&i2s_cfg);
    i2s_stream_set_clk(s_player->mixer.element_i2s, SAMPLERATE, 16, ESP_DOWNMIX_OUTPUT_TYPE_ONE_CHANNEL);

    //Equalizer
    app_player_load_eq_from_nvs(s_player->mixer.eq_gains);
    equalizer_cfg_t eq_cfg = DEFAULT_EQUALIZER_CONFIG();
    eq_cfg.channel = 1;
    eq_cfg.samplerate = SAMPLERATE;
    eq_cfg.set_gain = s_player->mixer.eq_gains;
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
    evt_cfg.internal_queue_size = 40;
    evt_cfg.external_queue_size = 40;
    s_player->evt = audio_event_iface_init(&evt_cfg);

    audio_pipeline_set_listener(s_player->mixer.pipeline, s_player->evt);
    downmix_set_output_type(s_player->mixer.element_mixer, ESP_DOWNMIX_OUTPUT_TYPE_ONE_CHANNEL);
    i2s_stream_set_clk(s_player->mixer.element_i2s, SAMPLERATE, 16, ESP_DOWNMIX_OUTPUT_TYPE_ONE_CHANNEL);

    // audio_pipeline_run(player->mixer.element_mixer);

    
    // audio_pipeline_run(player->sdcard_audio.pipeline);
    // app_player_sdcard_init(s_player->evt);

    audio_pipeline_run(s_player->mixer.pipeline);

    downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_BYPASS);

    downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
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
    s_player->current_base_pipeline = s_player->sdcard_audio.pipeline;
    // audio_pipeline_run(player->sdcard_audio.pipeline);

    ESP_LOGD(TAG, "player->evt:%d",(int)s_player->evt);
    ESP_LOGD(TAG, "player->mixer.pipeline:%d,mixer:%d,i2s:%d",(int)s_player->mixer.pipeline,(int)s_player->mixer.element_mixer,(int)s_player->mixer.element_i2s);

    ESP_LOGD(TAG, "player->tone_audio.pipeline:%d",(int)s_player->tone_audio.pipeline);
    ESP_LOGD(TAG, "player->tone_audio.element_flash_stream:%d",(int)s_player->tone_audio.element_flash_stream);
    ESP_LOGD(TAG, "player->tone_audio.element_decoder:%d",(int)s_player->tone_audio.element_decoder);
    ESP_LOGD(TAG, "player->tone_audio.element_rsp_filter:%d",(int)s_player->tone_audio.element_rsp_filter);
    ESP_LOGD(TAG, "player->tone_audio.element_raw:%d",(int)s_player->tone_audio.element_raw);

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

void app_player_embed_tone_play(uint8_t tone_type)
{
    return;// disable tone play
    if (tone_type >= TONE_URL_MAX)
    {
        return;
    }
    // audio_pipeline_stop(s_player->tone_audio.pipeline);
    // audio_pipeline_wait_for_stop(s_player->tone_audio.pipeline);
    // audio_pipeline_terminate(s_player->tone_audio.pipeline);
    ESP_LOGI(TAG, "Tone music start run, num: %d, url: %s", tone_type,embed_tone_url[tone_type]);
    audio_element_set_uri(s_player->tone_audio.element_flash_stream, embed_tone_url[tone_type]);
    // audio_pipeline_reset_ringbuffer(s_player->tone_audio.pipeline);
    // audio_pipeline_reset_elements(s_player->tone_audio.pipeline);
    audio_pipeline_run(s_player->tone_audio.pipeline);
    downmix_set_work_mode(s_player->mixer.element_mixer, ESP_DOWNMIX_WORK_MODE_SWITCH_ON);
    downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, MIX_INDEX_TONE_STREAM);
    ESP_LOGI(TAG, "Tone music running...");
}

typedef enum
{
    PLAY_ACITN_NONE = 0,
    PLAY_ACITN_PLAY,
    PLAY_ACITN_PAUSE,
} player_action_t;
uint8_t last_action = PLAY_ACITN_NONE;

void player_check_mixer_is_normal(){
    if((audio_element_get_state(s_player->mixer.element_i2s) != AEL_STATE_RUNNING) 
        || (audio_element_get_state(s_player->mixer.element_mixer) != AEL_STATE_RUNNING) 
        || (audio_element_get_state(s_player->mixer.element_equalizer) != AEL_STATE_RUNNING)){
        ESP_LOGE(TAG, "[player_check_mixer_is_normal] mixer i2s not running");
        // audio_pipeline_stop(s_player->mixer.pipeline);
        // audio_pipeline_wait_for_stop(s_player->mixer.pipeline);
        // audio_pipeline_terminate(s_player->mixer.pipeline);
        
        audio_pipeline_reset_ringbuffer(s_player->mixer.pipeline);
        audio_pipeline_reset_elements(s_player->mixer.pipeline);        
        audio_pipeline_change_state(s_player->mixer.pipeline,AEL_STATE_INIT);
        audio_pipeline_run(s_player->mixer.pipeline);
    }
}

void player_check_state_task(){

}



void app_player_on_play()
{
    //check mixer pipeline
    

    //no player source
    if(s_player->current_base_pipeline == NULL){
        ESP_LOGW(TAG, "[app_player_on_play] current_base_pipeline is null ");
        return;
    }
    //when plying bt audio
    if( s_player->current_base_pipeline != s_player->sdcard_audio.pipeline && esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED){
        ESP_LOGE(TAG, "[app_player_on_play] Bluetooth controller not enabled");
        app_switch2_sdcard();
    }
    if(s_player->current_base_pipeline == s_player->a2dp_audio.pipeline){
        if( s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED ){          
            if(s_bt_link.audio_state == ESP_A2D_AUDIO_STATE_REMOTE_SUSPEND || s_bt_link.audio_state == ESP_A2D_AUDIO_STATE_STOPPED
                ||  last_action == PLAY_ACITN_PAUSE
                )
            {

                ESP_LOGI(TAG, "[app_player_on_play] A2DP not started, start it");
                periph_bt_play(app_bt_get_periph());
                last_action = PLAY_ACITN_PLAY;

            }else {

                ESP_LOGI(TAG, "[app_player_on_play] A2DP started, pause it");                
                periph_bt_pause(app_bt_get_periph());
                last_action = PLAY_ACITN_PAUSE;
                
            }
        }
        else{
            ESP_LOGE(TAG, "[app_player_on_play] A2DP not connected ");
        }
        return;
    }
    // when playing sdcard music
    else if( s_player->current_base_pipeline == s_player->sdcard_audio.pipeline ){      
        //check lock
        if( s_player->sdcard_audio.operator_lock ){
            ESP_LOGW(TAG, "[app_player_on_play] sdcard_audio open lock");
            return;
        }
        audio_element_state_t el_state = audio_pipeline_get_state(s_player->sdcard_audio.pipeline);
        
        // audio_element_state_t el_state = audio_element_get_state(player->sdcard_audio.element_fatfs);
        ESP_LOGI(TAG, "[app_player_on_play] [app_player_on_play] pipe state: %d", el_state);
        switch (el_state)
        {
        case AEL_STATE_INIT:
            ESP_LOGI(TAG, "[app_player_on_play] Starting audio pipeline");
            audio_pipeline_run(s_player->current_base_pipeline);
            set_play_state_check_timer(AEL_STATE_RUNNING);
            break;
        case AEL_STATE_RUNNING:
            if(s_player->block_pause) break;
            ESP_LOGI(TAG, "[app_player_on_play] Pausing audio pipeline");
            audio_element_pause(s_player->sdcard_audio.element_fatfs);
            // audio_pipeline_pause(s_player->sdcard_audio.pipeline);
            
            audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_PAUSED);
            set_play_state_check_timer(AEL_STATE_PAUSED);
            s_player->block_pause = true;     
            break;
        case AEL_STATE_PAUSED:
            if(s_player->block_pause) break;
            ESP_LOGI(TAG, "[app_player_on_play] Resuming audio pipeline");
            if(ESP_OK != audio_pipeline_resume(s_player->sdcard_audio.pipeline)){
                
            }
            // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
            set_play_state_check_timer(AEL_STATE_RUNNING);

            s_player->block_pause = true; 
            break;
        case AEL_STATE_STOPPED:
            // if(ESP_OK != audio_pipeline_resume(s_player->sdcard_audio.pipeline)){
            //     ESP_LOGE(TAG, "[ * ] Audio pipeline resume filed");
            //     audio_pipeline_stop(s_player->sdcard_audio.pipeline);
            //     audio_pipeline_wait_for_stop(s_player->sdcard_audio.pipeline);
            // }
            audio_pipeline_run(s_player->sdcard_audio.pipeline);
            // audio_pipeline_check_items_state(s_player->sdcard_audio.pipeline, s_player->sdcard_audio.element_fatfs,AEL_STATE_RUNNING);
            // audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_RUNNING);
            // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
            set_play_state_check_timer(AEL_STATE_RUNNING);  
            break;
        default:
            ESP_LOGE(TAG, "[app_player_on_play] app_player_on_play: Not supported state %d, change to init", el_state);
            audio_pipeline_reset_ringbuffer(s_player->current_base_pipeline);
            audio_pipeline_reset_elements(s_player->current_base_pipeline);
            audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_INIT);
            audio_pipeline_run(s_player->sdcard_audio.pipeline);
            set_play_state_check_timer(AEL_STATE_RUNNING);      
        }
        
    }
    else{
        ESP_LOGE(TAG, "[app_player_on_play] Not support audio source ");
    }
    last_action = PLAY_ACITN_NONE;
}

void app_player_on_next(){

    ESP_LOGD(TAG, "[app_player_on_next] ");
    //no player source
    if(s_player->current_base_pipeline == NULL){
        ESP_LOGW(TAG, "[app_player_on_next] current_base_pipeline is null ");
        return;
    }
    //Bluetooth
    if(s_player->current_base_pipeline == s_player->a2dp_audio.pipeline){  
        if( s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED ){
            ESP_LOGI(TAG, "[app_player_on_next] A2DP next song");
            periph_bt_avrc_next(app_bt_get_periph());
        }
        else{
            ESP_LOGE(TAG, "[app_player_on_next] A2DP not connected ");
        }
    }
    //SDcard
    else if( s_player->current_base_pipeline == s_player->sdcard_audio.pipeline){
        if( s_player->sdcard_audio.operator_lock ){
            ESP_LOGI(TAG, "[app_player_on_next] sdcard_audio open lock");
            return;
        }

        ESP_LOGI(TAG, "[app_player_on_next] Stopped, advancing to the next song");
        char *url = NULL;
        // audio_pipeline_stop(s_player->current_base_pipeline);
        // audio_pipeline_wait_for_stop(s_player->current_base_pipeline);
        audio_pipeline_terminate(s_player->sdcard_audio.pipeline);


        audio_pipeline_reset_ringbuffer(s_player->sdcard_audio.pipeline);
        audio_pipeline_reset_elements(s_player->sdcard_audio.pipeline);
        sdcard_list_next(s_player->sdcard_audio.sdcard_list, 1, &url);
        if(url == NULL){
            ESP_LOGE(TAG, "[app_player_on_next] sdcard_list_next failed");
            return;
        }
        audio_element_set_uri(s_player->sdcard_audio.element_fatfs, url);

        uint16_t num = sdcard_list_get_url_id(s_player->sdcard_audio.sdcard_list);
        board_display_set_song_num(num);

        audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_INIT);
        // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
        audio_pipeline_run(s_player->sdcard_audio.pipeline);
        set_play_state_check_timer(AEL_STATE_RUNNING);
        ESP_LOGW(TAG, "[app_player_on_next] URL: %s", url);
        
    }
    else{
        ESP_LOGE(TAG, "[app_player_on_next] app_player_on_next: Not support audio source ");
    }
}
void app_player_on_prev()
{
    ESP_LOGD(TAG, "[app_player_on_prev] ");
    //no player source
    if(s_player->current_base_pipeline == NULL){
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
    else if (s_player->current_base_pipeline == s_player->sdcard_audio.pipeline)
    {

        if( s_player->sdcard_audio.operator_lock ){
            ESP_LOGI(TAG, "[app_player_on_prev] sdcard_audio open lock");
            return;
        }
        ESP_LOGI(TAG, "[ app_player_on_prev ] Stopped, going back to the previous song");
        char *url = NULL;
        // audio_pipeline_stop(s_player->current_base_pipeline);
        // audio_pipeline_wait_for_stop(s_player->current_base_pipeline);
        audio_pipeline_terminate(s_player->sdcard_audio.pipeline);
        audio_pipeline_reset_ringbuffer(s_player->sdcard_audio.pipeline);
        audio_pipeline_reset_elements(s_player->sdcard_audio.pipeline);


        sdcard_list_prev(s_player->sdcard_audio.sdcard_list, 1, &url);
        if(url == NULL){
            ESP_LOGE(TAG, "[app_player_on_prev] sdcard_list_prev failed");
            return;
        }
        audio_element_set_uri(s_player->sdcard_audio.element_fatfs, url);
        uint16_t num = sdcard_list_get_url_id(s_player->sdcard_audio.sdcard_list);
        board_display_set_song_num(num);
        audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_INIT);
        // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
        audio_pipeline_run(s_player->sdcard_audio.pipeline);
        
        set_play_state_check_timer(AEL_STATE_RUNNING);
        ESP_LOGW(TAG, "[ app_player_on_prev] URL: %s", url);
        
    }
    else
    {
        ESP_LOGE(TAG, "[*] app_player_on_prev: Not support audio source");
    }
}

void app_player_on_volume_up()
{
    if(s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED){
        periph_bt_volume_up(app_bt_get_periph());     
        ESP_LOGI(TAG, "[ * ] Remote volume+");
    }
    else {
        int player_volume;
        audio_hal_get_volume(s_player->board_handle->audio_hal, &player_volume);
        player_volume += 10;
        if (player_volume > 100) {
            player_volume = 100;
        }
        audio_hal_set_volume(s_player->board_handle->audio_hal, player_volume);
        ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
    }
}
void app_player_on_volume_down(){
    if(s_bt_link.conn_state == ESP_A2D_CONNECTION_STATE_CONNECTED){        
        periph_bt_volume_down(app_bt_get_periph());        
        ESP_LOGI(TAG, "[ * ] Remote volume-");
    }
    else {
        int player_volume;
        audio_hal_get_volume(s_player->board_handle->audio_hal, &player_volume);
        player_volume -= 10;
        if (player_volume < 0) {
            player_volume = 0;
        }
        audio_hal_set_volume(s_player->board_handle->audio_hal, player_volume);
        ESP_LOGI(TAG, "[ * ] Volume set to %d %%", player_volume);
    }
}

void audio_player_show_element_status(){
    ESP_LOGD(TAG,"\r\n-----------Element status:--------------- \r\n");
    if(s_player){
        ESP_LOGD(TAG, "current_base_pipeline:[%4x]",(unsigned int)s_player->current_base_pipeline);        
        // 检查mixer pipeline及其元素是否有效
        if(s_player->mixer.pipeline) {
            ESP_LOGD(TAG, "mixer[%4x]pipeline:%d,mixer:%d,eq:%d i2s:%d",
                (unsigned int)s_player->mixer.pipeline,
                s_player->mixer.pipeline ? audio_pipeline_get_state(s_player->mixer.pipeline) : -1,
                s_player->mixer.element_mixer ? audio_element_get_state(s_player->mixer.element_mixer) : -1,
                s_player->mixer.element_equalizer ? audio_element_get_state(s_player->mixer.element_equalizer) : -1,
                s_player->mixer.element_i2s ? audio_element_get_state(s_player->mixer.element_i2s) : -1);
        }

        // 检查sdcard pipeline及其元素是否有效
        if(s_player->sdcard_audio.pipeline) {
            ESP_LOGD(TAG, "sdcard[%4x].pipeline:%d,fatfs:%d,decoder:%d,rsp_filter:%d,raw:%d",
                (unsigned int)s_player->sdcard_audio.pipeline,
                s_player->sdcard_audio.pipeline ? audio_pipeline_get_state(s_player->sdcard_audio.pipeline) : -1,
                s_player->sdcard_audio.element_fatfs ? audio_element_get_state(s_player->sdcard_audio.element_fatfs) : -1,
                s_player->sdcard_audio.element_decoder ? audio_element_get_state(s_player->sdcard_audio.element_decoder) : -1,
                s_player->sdcard_audio.element_rsp_filter ? audio_element_get_state(s_player->sdcard_audio.element_rsp_filter) : -1,
                s_player->sdcard_audio.element_raw ? audio_element_get_state(s_player->sdcard_audio.element_raw) : -1);
        }

        // 检查a2dp pipeline及其元素是否有效
        if(s_player->a2dp_audio.pipeline) {
            ESP_LOGD(TAG, "a2dp[%4x].pipeline:%d,a2dp_stream:%d,rsp_filter:%d,raw:%d",
                (unsigned int)s_player->a2dp_audio.pipeline,
                s_player->a2dp_audio.pipeline ? audio_pipeline_get_state(s_player->a2dp_audio.pipeline) : -1,
                s_player->a2dp_audio.element_a2dp_stream ? audio_element_get_state(s_player->a2dp_audio.element_a2dp_stream) : -1,
                s_player->a2dp_audio.element_rsp_filter ? audio_element_get_state(s_player->a2dp_audio.element_rsp_filter) : -1,
                s_player->a2dp_audio.element_raw ? audio_element_get_state(s_player->a2dp_audio.element_raw) : -1);
        }
    }
}

esp_err_t app_player_StopSdcardPipeline(){
    if(s_player->sdcard_audio.pipeline){
        esp_err_t ret;
        ret = audio_pipeline_stop(s_player->sdcard_audio.pipeline);
        ret = audio_pipeline_wait_for_stop(s_player->sdcard_audio.pipeline);
        audio_pipeline_terminate(s_player->sdcard_audio.pipeline);
        if(s_player->current_base_pipeline == s_player->sdcard_audio.pipeline){
            s_player->current_base_pipeline = NULL;
        }
        return ret;
    }
    else {
        ESP_LOGW(TAG, "[app_player_StopSdcardPipeline] sdcard_audio.pipeline is null ");

    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_player_sdcard_on_unmount()
{
    if (s_player->sdcard_audio.pipeline)
    {
        esp_err_t ret;
        if(s_player->current_base_pipeline == s_player->sdcard_audio.pipeline){
            s_player->current_base_pipeline = NULL;
        }
        // audio_pipeline_stop(s_player->sdcard_audio.pipeline);
        // audio_pipeline_wait_for_stop(s_player->sdcard_audio.pipeline);
        audio_pipeline_terminate(s_player->sdcard_audio.pipeline);
        audio_pipeline_reset_ringbuffer(s_player->sdcard_audio.pipeline);
        audio_pipeline_reset_elements(s_player->sdcard_audio.pipeline);
        audio_pipeline_change_state(s_player->sdcard_audio.pipeline,AEL_STATE_INIT);
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t app_player_sdcard_on_mount(){
    if (s_player->current_base_pipeline == NULL){
        app_switch2_sdcard();
    }
    return ESP_OK;
}