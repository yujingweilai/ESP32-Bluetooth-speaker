#ifndef _APP_PLAYER_H_
#define _APP_PLAYER_H_
#include "audio_def.h"
#include "board.h"
audio_err_t app_player_init(audio_board_handle_t board_handle);
audio_err_t app_player_a2dp_init();
audio_err_t app_player_a2dp_deinit();
bool app_player_is_a2dp_connected();
bool app_player_is_sdcard_playing();

void app_player_on_play();
void app_player_on_next();
void app_player_on_prev();
void audio_player_show_element_status();
void app_player_on_volume_down();
void app_player_on_volume_up();
void app_player_embed_tone_play(uint8_t tone_type);
esp_err_t app_player_StopSdcardPipeline();
void app_switch2_sdcard();
const int *app_player_get_eq();
esp_err_t app_player_set_eq(int *eq_gains);
esp_err_t app_player_a2dp_disconnect();
void app_player_a2dp_auto_reconnect_start();
void app_player_a2dp_auto_reconnect_stop();
esp_err_t app_player_set_base_stream_silence_wait(bool enable);
esp_err_t app_player_set_base_stream_read_freeze(bool enable);
esp_err_t app_player_get_user_volume(int *volume);
esp_err_t app_player_set_base_stream_low_water_duck(bool enable, int limit);
esp_err_t app_player_play_shutdown_tone(uint8_t tone_type, uint32_t timeout_ms);
esp_err_t app_player_prepare_shutdown(void);



esp_err_t app_player_sdcard_on_unmount();
esp_err_t app_player_on_mount();



#endif // _APP_PLAYER_H_
