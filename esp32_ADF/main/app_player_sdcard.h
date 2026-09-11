#ifndef __APP_PLAYER_SDCARD_H__
#define __APP_PLAYER_SDCARD_H__

#include "audio_pipeline.h"

//sdcard 外部instance， 提供播放，暂停，停止，下一首，上一首，音量调节，等接口
typedef struct{
    void (*play_pause)();
    void (*pause)();
    void (*next)();
    void (*prev)();
    // void (*reset)();
    void (*stop)();
    ringbuf_handle_t (*get_ringbuf)();
    bool (*is_playing)();
} sdcard_player_t; 
sdcard_player_t *app_player_sdcard_get_interface();
esp_err_t app_player_sdcard_init(audio_event_iface_handle_t evt);
void app_player_sdcard_print_state();
int app_player_sdcard_get_display_track_num(void);
esp_err_t app_player_sdcard_refresh_track_display(void);
enum{
    SDCARD_PLAYER_EVENT_PLAY_STARTED,
    SDCARD_PLAYER_EVENT_PLAY_PAUSED,
    SDCARD_PLAYER_EVENT_PLAY_RESUMED,
    SDCARD_PLAYER_EVENT_PLAY_STOPPED,
    SDCARD_PLAYER_EVENT_PLAY_FINISHED,
    SDCARD_PLAYER_EVENT_TRACK_CHANGED,
    SDCARD_PLAYER_EVENT_ERROR
};



#endif

