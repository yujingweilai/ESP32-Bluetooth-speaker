#include "app_player_sdcard.h"

#include "periph_sdcard.h"
#include "fatfs_stream.h"
#include "a2dp_stream.h"

#include "dram_list.h"
#include "playlist.h"
#include "sdcard_scan.h"

#include "audio_pipeline.h"
#include "audio_thread.h"
#include "audio_event_iface.h"

#include "filter_resample.h"
#include "esp_decoder.h"
#include "mp3_decoder.h"
#include "raw_stream.h"
#include "audio_def.h"
#include "audio_mem.h"
#include "audio_thread.h"
#include "audio_mutex.h"
#include "ringbuf.h"
#include "freertos/task.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "board.h"
#include "fw_timer.h"
#include "app_player.h"

#define OUTPUT_SAMPLERATE 48000

/*
 * ============================================================================
 * DEBUG: Event Queue for Music Control
 * ============================================================================
 * The music control commands (play, pause, next, prev, stop) are now handled
 * via a FreeRTOS queue. This decouples the caller context from the actual
 * pipeline operations, improving thread safety and responsiveness.
 *
 * Flow:
 * 1. External call (e.g., on_next_song()) posts a command to cmd_queue.
 * 2. sdcard_cmd_task() receives the command and executes the corresponding
 *    internal function (e.g., _on_next_song()).
 * ============================================================================
 */

// DEBUG: Command types for the event queue
typedef enum {
    SDCARD_CMD_NEXT,         // Go to next song
    SDCARD_CMD_PREV,         // Go to previous song
    SDCARD_CMD_NEXT_FORCE,   // Internal next without debounce
    SDCARD_CMD_SWITCH_COMMIT,// Commit pending switch steps
    SDCARD_CMD_PLAY_PAUSE,   // Toggle play/pause
    SDCARD_CMD_STOP,         // Stop playback
    SDCARD_CMD_PAUSE,        // Pause playback
    SDCARD_CMD_STATE_CHECK,  // Check play state (from timer callback)
} sdcard_cmd_t;

// DEBUG: Log string for commands (for debugging)
static const char *cmd_log_str[] = {
    "SDCARD_CMD_NEXT",
    "SDCARD_CMD_PREV",
    "SDCARD_CMD_NEXT_FORCE",
    "SDCARD_CMD_SWITCH_COMMIT",
    "SDCARD_CMD_PLAY_PAUSE",
    "SDCARD_CMD_STOP",
    "SDCARD_CMD_PAUSE",
    "SDCARD_CMD_STATE_CHECK",
};

#define SDCARD_CMD_QUEUE_SIZE 16  // DEBUG: Queue depth for pending commands
#define SDCARD_CMD_QUEUE_RECOVER_THRESHOLD 12

// player state
typedef enum
{
    SD_PLAYER_STATE_INIT,
    SD_PLAYER_STATE_SWITCHING,
    SD_PLAYER_STATE_LOADING,
    SD_PLAYER_STATE_PLAYING,
    SD_PLAYER_STATE_PAUSED,
    SD_PLAYER_STATE_STOPPED,
    SD_PLAYER_STATE_ERROR,
} sdcard_state_t;
// state log str
static const char *state_log_str[] = {
    "SD_PLAYER_STATE_INIT",
    "SD_PLAYER_STATE_SWITCHING",
    "SD_PLAYER_STATE_LOADING",
    "SD_PLAYER_STATE_PLAYING",
    "SD_PLAYER_STATE_PAUSED",
    "SD_PLAYER_STATE_STOPPED",
    "SD_PLAYER_STATE_ERROR",
};

static const char *TAG = "APP_PLAYER_SDCARD";

/*
 * ====================== 可配置缓存诊断宏 ======================
 * 1) SDCARD_RB_DIAG_ENABLE:
 *    总开关。置 1 开启缓存诊断日志，置 0 完全关闭，便于量产时减小日志开销。
 * 2) SDCARD_RB_DIAG_ON_PAUSE:
 *    暂停路径日志开关。用于定位“按下暂停后延迟停下”的问题。
 * 3) SDCARD_RB_DIAG_ON_TRACK_SWITCH:
 *    切歌路径日志开关。用于观察 next/prev 时是否有旧缓存残留。
 * 4) SDCARD_RB_DIAG_WARN_ENABLE:
 *    阈值告警开关。置 1 后，当 filled/size 超过阈值时自动打印 ESP_LOGW。
 * 5) SDCARD_RB_DIAG_WARN_THRESHOLD_PERCENT:
 *    阈值百分比（0~100）。例如 80 表示缓存占用超过 80% 触发告警。
 * 6) SDCARD_RB_DIAG_ON_LOW_WATER:
 *    低水位保护关键节点日志开关。用于观察供数不足时各级 ringbuf 水位。
 * 7) SDCARD_SD_READ_COST_LOG_ENABLE:
 *    SD 供数耗时诊断开关。日志统一带 '#' 标记，便于串口抓取过滤。
 */
#define SDCARD_RB_DIAG_ENABLE            1
#define SDCARD_RB_DIAG_ON_PAUSE          1
#define SDCARD_RB_DIAG_ON_TRACK_SWITCH   1
#define SDCARD_RB_DIAG_WARN_ENABLE       1
#define SDCARD_RB_DIAG_ON_LOW_WATER      1
#define SDCARD_RB_DIAG_WARN_THRESHOLD_PERCENT 80
#define SDCARD_SD_READ_COST_LOG_ENABLE   1
#define SDCARD_SD_READ_COST_WARN_MS      800U
#define SDCARD_WAV_FAST_CHECK_BYTES      4096U
#define SDCARD_LAST_URL_MAX_LEN          159U
#define SDCARD_LAST_REASON_MAX_LEN       32U
#define SDCARD_ACTIVE_URL_MAX_LEN        260U
#define SDCARD_MAX_FILENAME_BYTES        168U//扫描歌曲名称最大文件名字节限制
#define SDCARD_WAV_SHADOW_PATH           "/sdcard/_vib_shadow.wav"
#define SDCARD_WAV_BYPASS_THRESHOLD_BYTES 4096U
#define SDCARD_MP3_SCAN_CHUNK_BYTES      1024U
#define SDCARD_MP3_HEAD_SCAN_LIMIT_BYTES (64U * 1024U)
#define SDCARD_MP3_MAX_DIRTY_HEAD_BYTES  (512U * 1024U)
#define SDCARD_STARTUP_RESUME_TIMEOUT_MS 2000
#define SDCARD_LOADING_TIMEOUT_MS        20000U
#define SDCARD_LOADING_STALE_RECOVER_MS  1500U
/*
 * 同类型歌曲切换策略开关：
 * 0: 默认全量重置 pipeline，优先保证稳定性。
 * 1: 同类型(MP3->MP3 / WAV->WAV)允许热切，减少切歌等待。
 */
#define SDCARD_HOT_SWITCH_ON_SAME_TYPE   0
#define SDCARD_MP3_PRELOAD_THRESHOLD     0.18f
#define SDCARD_WAV_PRELOAD_THRESHOLD     0.85f
#define SDCARD_DECODER_TASK_PRIO_DEFAULT 12
#define SDCARD_WAV_FATFS_TASK_PRIO       14 // 提高 FatFs 的优先级，确保优先抢占 CPU 读取 SD 卡数据
#define SDCARD_TIMER_ID_SWITCH_COMMIT    0x5301U
#define SDCARD_SWITCH_DEBOUNCE_MS        200U
#define SDCARD_AUTO_NEXT_DRAIN_CHECK_MS  30U
#define SDCARD_AUTO_NEXT_DRAIN_TIMEOUT_MS 7000U
#define SDCARD_AUTO_NEXT_DRAIN_THRESHOLD_BYTES (8 * 1024)
#define SDCARD_AUTO_NEXT_DRAIN_STABLE_HITS 2U
#define SDCARD_SCAN_MAX_TRACKS           999U//扫描歌曲最大数量限制
#define SDCARD_MAX_AUDIO_FILE_SIZE_BYTES (128U * 1024U * 1024U)
#define SDCARD_CACHE_PARTITION_LABEL     "playlist"
#define SDCARD_CACHE_BASE_PATH           "/spiffs"
#define SDCARD_CACHE_M3U_PATH            "/spiffs/music.m3u"
#define SDCARD_CACHE_IDX_PATH            "/spiffs/music.idx"
#define SDCARD_CACHE_M3U_STAGE_PATH      "/spiffs/music_stage.m3u"
#define SDCARD_CACHE_IDX_STAGE_PATH      "/spiffs/music_stage.idx"
#define SDCARD_CACHE_M3U_TMP_PATH        "/spiffs/music.tmp"
#define SDCARD_CACHE_IDX_TMP_PATH        "/spiffs/music_tmp.idx"
#define SDCARD_CACHE_M3U_MP3_TMP_PATH    "/spiffs/music_mp3.tmp"
#define SDCARD_CACHE_M3U_WAV_TMP_PATH    "/spiffs/music_wav.tmp"
#define SDCARD_CACHE_M3U_OTHER_TMP_PATH  "/spiffs/music_other.tmp"
#define SDCARD_CACHE_IDX_MAGIC           "VIBMIDX"
#define SDCARD_CACHE_IDX_VERSION         0x00010002UL

/* 低水位守护任务的轮询周期。100ms 足够快，且不会给系统带来明显额外负担。 */
#define SDCARD_LOW_WATER_CHECK_MS        100U
/* 启动预缓存的复查周期。每次复查输出 RB 是否已经达到安全水位。 */
#define SDCARD_STARTUP_GUARD_CHECK_MS    120U
/* 启动预缓存时，连续命中安全水位的次数要求，避免刚到阈值就立刻放音。 */
#define SDCARD_STARTUP_STABLE_HITS       3U
/* 运行期进入低水位保护的基础门限。filled 低于此值，认为供数已不安全。 */
#define SDCARD_LOW_WATER_MIN_LEVEL       (128 * 1024)
/* 运行期退出低水位保护的安全门限。必须恢复到该水位附近才允许平滑放开。 */
#define SDCARD_LOW_WATER_SAFE_LEVEL      (320 * 1024)
/* MP3 首播时允许的最小安全水位。MP3 解码后更容易较快补仓，因此门限低于 WAV。 */
#define SDCARD_MP3_STARTUP_SAFE_MIN_LEVEL (24 * 1024)
/* MP3 运行期低水位进入门限的附加量，在首播安全值基础上再抬高一点。 */
#define SDCARD_MP3_LOW_WATER_MIN_EXTRA   (16 * 1024)
/* MP3 运行期恢复门限的附加量，用于形成进入/退出迟滞，避免来回抖动。 */
#define SDCARD_MP3_LOW_WATER_SAFE_EXTRA  (48 * 1024)
/* 当 safe_level 过低时，至少拉开的一段安全余量，避免 min/safe 几乎重叠。 */
#define SDCARD_LOW_WATER_SILENCE_CHUNK   (12 * 1024)
/* WAV/通用低水位进入保护前，连续命中低水位的次数。 */
#define SDCARD_LOW_WATER_ENTER_HITS      3U
/* WAV/通用低水位恢复前，连续命中安全水位的次数。 */
#define SDCARD_LOW_WATER_RECOVER_HITS    2U
/* MP3 进入保护前要求更多命中次数，减少瞬时抖动导致的误触发。 */
#define SDCARD_MP3_LOW_WATER_ENTER_HITS  4U
/* MP3 恢复前同样要求更多命中次数，避免刚恢复就再次跌破。 */
#define SDCARD_MP3_LOW_WATER_RECOVER_HITS 3U
/* 低水位保护最长等待时间。超过该时间仍无法补仓，则强制切下一首自救。 */
#define SDCARD_LOW_WATER_TIMEOUT_MS      10000U
/* 低水位 duck 渐变步进。每轮减少/恢复多少音量，兼顾听感与响应速度。 */
#define SDCARD_LOW_WATER_DUCK_STEP       5
#define SDCARD_SCAN_TASK_STACK_SIZE      (12 * 1024)

typedef struct
{
    uint32_t mp3_check_count;
    uint32_t mp3_check_fail_count;
    uint32_t mp3_skip_count;
    uint32_t wav_check_count;
    uint32_t wav_check_fail_count;
    uint32_t wav_skip_count;
    uint32_t next_request_count;
    uint32_t next_drop_count;
    uint32_t prev_request_count;
    uint32_t prev_drop_count;
    uint32_t loading_retry_count;
    uint32_t loading_retry_escalate_count;
    uint32_t decoder_error_count;
    char last_mp3_fail_reason[SDCARD_LAST_REASON_MAX_LEN];
    char last_mp3_fail_url[SDCARD_LAST_URL_MAX_LEN];
    char last_wav_fail_reason[SDCARD_LAST_REASON_MAX_LEN];
    char last_wav_fail_url[SDCARD_LAST_URL_MAX_LEN];
} sdcard_diag_stats_t;

typedef struct
{
    uint32_t audio_offset;
    uint32_t id3v2_size;
    bool has_id3v2;
} mp3_scan_info_t;

typedef struct
{
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    bool requires_shadow;
} wav_scan_info_t;

typedef struct
{
    char magic[8];
    uint32_t version;
    uint32_t track_count;
    uint32_t scan_signature;
    uint32_t root_file_count;
    uint32_t root_latest_mtime;
    uint64_t root_total_size;
    uint32_t root_signature;
} sdcard_cache_idx_header_t;

typedef struct
{
    uint32_t file_count;
    uint32_t latest_mtime;
    uint64_t total_size;
    uint32_t signature;
} sdcard_root_fingerprint_t;

typedef struct
{
    FILE *m3u_fp;
    FILE *idx_fp;
    FILE *mp3_fp;
    FILE *wav_fp;
    FILE *other_fp;
    uint32_t count;
    uint32_t limit;
    uint32_t signature;
    uint32_t mp3_count;
    uint32_t wav_count;
    uint32_t other_count;
    sdcard_root_fingerprint_t root_fp;
    uint32_t path_hashes[SDCARD_SCAN_MAX_TRACKS];
    uint32_t name_hashes[SDCARD_SCAN_MAX_TRACKS];
    /* 增量扫描模式：边扫边追加到 active M3U/IDX，扫到第 1 首即通知开播 */
    bool incremental_mode;
    bool first_track_notified;
    FILE *active_m3u_fp;
    FILE *active_idx_fp;
    uint32_t active_track_count;
} sdcard_scan_context_t;

typedef enum
{
    TRACK_FORMAT_UNKNOWN = 0,
    TRACK_FORMAT_MP3,
    TRACK_FORMAT_WAV,
    TRACK_FORMAT_OTHER,
} track_format_t;

typedef struct
{
    char **items;
    size_t count;
    size_t capacity;
} track_url_bucket_t;

typedef struct
{
    char *playlist_url;
    char playback_url[SDCARD_ACTIVE_URL_MAX_LEN];
    uint32_t start_offset;
    track_format_t format;
    bool is_wav_track;
    bool use_shadow_file;
    mp3_scan_info_t mp3_info;
    wav_scan_info_t wav_info;
} track_play_plan_t;

typedef enum
{
    SDCARD_PRIO_POLICY_DEFAULT = 0,
    SDCARD_PRIO_POLICY_WAV_PRODUCER_FIRST,
} sdcard_prio_policy_t;

typedef enum
{
    LOW_WATER_DUCK_STATE_IDLE = 0,
    LOW_WATER_DUCK_STATE_FADING_OUT,
    LOW_WATER_DUCK_STATE_WAIT_RECOVER,
    LOW_WATER_DUCK_STATE_FADING_IN,
} low_water_duck_state_t;

typedef struct
{
    audio_pipeline_handle_t pipeline;
    audio_element_handle_t element_fatfs;
    audio_element_handle_t element_decoder;
    audio_element_handle_t element_rsp_filter;
    audio_element_handle_t element_raw;
    playlist_handle_t playlist;
    sdcard_player_t interface;
    audio_thread_t sdcard_handler;
    ringbuf_handle_t output_rb;
    audio_event_iface_handle_t evt_out;
    sdcard_state_t state;
    xSemaphoreHandle lock;
    xQueueHandle cmd_queue;  // DEBUG: Queue for music control commands
    sdcard_diag_stats_t stats;
    bool cache_fs_ready;
    bool queue_recover_pending;
    uint32_t track_total;
    uint32_t track_index;
    uint32_t track_scan_signature;
    char current_playlist_url[SDCARD_ACTIVE_URL_MAX_LEN];
    char prev_playlist_url[SDCARD_ACTIVE_URL_MAX_LEN];
    char current_playback_url[SDCARD_ACTIVE_URL_MAX_LEN];
    uint32_t current_track_start_offset;
    char next_playlist_url[SDCARD_ACTIVE_URL_MAX_LEN];
    int32_t pending_switch_steps;
    bool switch_debounce_active;
    bool task_prio_baseline_valid;
    UBaseType_t fatfs_task_prio_baseline;
    UBaseType_t decoder_task_prio_baseline;
    UBaseType_t filter_task_prio_baseline;
    track_format_t current_track_format;
    bool current_track_is_wav;
    TickType_t current_track_play_tick;
    TickType_t loading_enter_tick;
    bool soft_switch_mute_active;
    bool startup_guard_active;
    bool resume_guard_active;
    bool pause_transition_active;
    TickType_t resume_guard_enter_tick;
    uint8_t startup_stable_hits;
    bool auto_next_pending;
    TickType_t auto_next_wait_tick;
    uint8_t auto_next_stable_hits;
    bool low_water_guard_active;
    uint8_t low_water_enter_hits;
    uint8_t low_water_recover_hits;
    low_water_duck_state_t low_water_duck_state;
    TickType_t low_water_enter_tick;
    int low_water_saved_volume;
    int low_water_duck_volume;
    TaskHandle_t scan_task_handle;
    bool scan_task_running;
    bool scan_activate_cache;
    bool cache_swap_pending;
} sdcard_audio_handler_t;

static sdcard_audio_handler_t *s_sdcard_handler = NULL;
/* 复用型 MP3 扫描缓冲，放在静态区避免启动阶段压 main 栈。 */
static uint8_t s_mp3_scan_buf[SDCARD_MP3_SCAN_CHUNK_BYTES];

static void set_state(sdcard_state_t state);

// DEBUG: Internal implementations (called by the command task)
static void _play_latest();
static void _on_next_song();
static void _on_prev_song();
static void _on_play_pause();
static void _on_stop();
static void _on_pause();
static void _pause_pipeline_fast(void);
static void _abort_pipeline_ringbufs(void);
static void _reset_pipeline_ringbufs_precise(void);
static void _recycle_pipeline_for_track_switch(void);
static void _flush_pause_tail_pcm(void);
static void _dump_pipeline_cache_status(const char *stage);
static void _rb_diag_warn_if_needed(const char *stage, const char *rb_name, ringbuf_handle_t rb);
static void _log_sd_read_cost(const char *stage, TickType_t start_tick, int filled, int rb_size, int safe_level);
static void _log_output_rb_watermark(const char *stage, int filled, int rb_size, int min_level, int safe_level);
static bool _is_wav_url(const char *url);
static bool _is_mp3_url(const char *url);
static track_format_t _classify_track_format(const char *url);
static esp_err_t _ensure_cache_fs_ready(void);
static uint32_t _fnv1a_update(uint32_t hash, const void *data, size_t len);
static uint32_t _hash_text(const char *text);
static const char *_path_basename(const char *path);
static uint32_t _get_loading_timeout_ms(void);
static bool _is_scan_hidden_name(const char *name);
static bool _has_invalid_filename_char(const char *name);
static bool _is_scan_temp_name(const char *name);
static bool _is_supported_audio_file(const char *name);
static bool _is_duplicate_hash(const uint32_t *list, uint32_t count, uint32_t hash);
static esp_err_t _collect_root_fingerprint(const char *dir_path, sdcard_root_fingerprint_t *fingerprint);
static bool _is_root_fingerprint_match(const sdcard_cache_idx_header_t *header,
                                       const sdcard_root_fingerprint_t *fingerprint);
static esp_err_t _load_track_cache_from_spiffs(void);
static esp_err_t _restore_cached_tracks(bool *out_need_rebuild);
static esp_err_t _prepare_track_cache_on_startup(void);
static esp_err_t _cache_read_header(sdcard_cache_idx_header_t *header);
static esp_err_t _cache_read_track_url_by_index(uint32_t index, char *url, size_t url_size);
static esp_err_t _cache_refresh_neighbors(void);
static esp_err_t _cache_set_current_index(uint32_t index);
static esp_err_t _find_track_index_in_cache_m3u(const char *m3u_path, const char *target_url, uint32_t *out_index);
static esp_err_t _apply_pending_cache_if_needed(void);
static esp_err_t _scan_directory_to_cache(const char *dir_path, sdcard_scan_context_t *ctx);
static esp_err_t _rebuild_track_cache(bool activate_now);
static FILE *_select_scan_bucket_file(sdcard_scan_context_t *ctx, const char *path, track_format_t *out_format);
static esp_err_t _merge_scan_bucket_to_cache(FILE *src_fp, sdcard_scan_context_t *ctx, uint32_t *merged_count, const char *skip_url);
static esp_err_t _scan_mp3_file(const char *url, mp3_scan_info_t *info);
static esp_err_t _scan_wav_file(const char *url, wav_scan_info_t *info);
static esp_err_t _prepare_mp3_play_plan(const char *url, track_play_plan_t *plan);
static esp_err_t _build_wav_shadow_file(const char *source_url, const wav_scan_info_t *info);
static esp_err_t _prepare_wav_play_plan(const char *url, track_play_plan_t *plan);
static esp_err_t _select_playable_track(track_play_plan_t *plan);
static bool _bucket_append_url(track_url_bucket_t *bucket, const char *url);
static void _bucket_release(track_url_bucket_t *bucket);
static esp_err_t _playlist_append_bucket(playlist_handle_t playlist, track_url_bucket_t *bucket);
static esp_err_t _create_playlist_handle(playlist_handle_t *out_playlist);
static esp_err_t _rebuild_playlist_by_format(playlist_handle_t *playlist_handle);
static int _get_display_track_num_locked(void);
static void _record_mp3_check_fail(const char *reason, const char *url);
static void _record_wav_check_fail(const char *reason, const char *url);
static void _log_diag_stats(const char *stage);
static void _clear_current_track_context(void);
static void _remember_current_track(const track_play_plan_t *plan);
static sdcard_prio_policy_t _get_track_prio_policy(const track_play_plan_t *plan);
static float _get_track_preload_threshold(const track_play_plan_t *plan);
static esp_err_t _apply_track_prio_policy(const track_play_plan_t *plan);
static esp_err_t _start_pipeline_for_plan(const track_play_plan_t *plan);
static esp_err_t _build_current_track_plan(track_play_plan_t *plan);
static const char *_uri_to_local_path(const char *url);
static bool _get_task_prio_by_name(const char *task_name, UBaseType_t *out_prio);
static bool _set_task_prio_by_name(const char *task_name, UBaseType_t target_prio);
static void _log_track_task_prio(const char *stage);
static void _request_switch_step(int32_t step, const char *source);
static void _commit_pending_switch(void);
static void _apply_playlist_offset(int32_t step);
static bool _should_use_hot_reset_for_current_track(void);
static void _hot_reset_pipeline_for_track_switch(void);
static void _handle_low_water_guard(void);
static int _calc_safe_buffer_level(int rb_size, int min_level, int safe_level);
static bool _wait_track_startup_buffer_ready(void);
static bool _handle_auto_next_wait(void);
static void _prepare_manual_soft_switch(void);
static bool _recover_stale_loading_state(const char *source);
static esp_err_t _schedule_loading_state_check(uint32_t delay_ms, const char *source);
static void switch_commit_timer_callback(U16 timerId, void *arg);
static void sdcard_low_water_guard_task(void *pv);
static void sdcard_scan_task(void *pv);
static void _on_next_song_force(void);
static void _reset_low_water_guard_state(bool keep_read_freeze, bool keep_silence_wait);
static esp_err_t _trigger_sdcard_scan_task(bool activate_now);
static esp_err_t _incremental_append_to_active_cache(sdcard_scan_context_t *ctx, const char *url);
static esp_err_t _incremental_finalize_cache(sdcard_scan_context_t *ctx);

// DEBUG: Public wrappers (post commands to the queue)
static void on_next_song();
static esp_err_t on_next_song_force(void);
static void on_prev_song();
static void on_play_pause();
void on_stop();
static void on_pause();

esp_err_t send_event_out(int cmd, void *data, int data_len)
{
    if (!s_sdcard_handler->evt_out)
    {
        return ESP_ERR_INVALID_STATE;
    }

    audio_event_iface_msg_t msg = {
        .source_type = AUDIO_ELEMENT_TYPE_PLAYER,
        .source = &(s_sdcard_handler->interface),
        .cmd = cmd,
        .data = data,
        .data_len = data_len};

    return audio_event_iface_sendout(s_sdcard_handler->evt_out, &msg);
}

static bool is_playing()
{
    if (s_sdcard_handler == NULL)
    {
        return false;
    }
    return s_sdcard_handler->state == SD_PLAYER_STATE_PLAYING ||
           s_sdcard_handler->state == SD_PLAYER_STATE_LOADING ||
           s_sdcard_handler->state == SD_PLAYER_STATE_SWITCHING;
}

static ringbuf_handle_t get_output_rb()
{
    return s_sdcard_handler->output_rb;
}

/*
 * 根据当前PSRAM余量为SD卡播放链路选择更稳妥的RB大小。
 * 仅作用于sdcard pipeline，避免影响A2DP/BLE/OLED等其余模块。
 */
static int get_sdcard_rb_size(void)
{
#ifdef CONFIG_SPIRAM
    int free_psram = (int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    int largest_psram = (int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

    /* 默认值：采用320KB方案，平衡系统多模块共存 */
    int rb_size = 384 * 1024;

    /*
     * 第一档：512KB 大buffer（平衡与高性能）
     * 条件分析：
     * - free_psram ≥ 1600KB
     * - largest_psram ≥ 900KB
     *
     * 适用时机：开机初期，给 A2DP/BLE 等链路预留充足空间
     */
    if (free_psram >= (1600 * 1024) && largest_psram >= (900 * 1024)) {
        rb_size = 512 * 1024;
    }
    /*
     * 第二档：384KB 中等buffer（稳健兼容）
     * 条件分析：
     * - free_psram ≥ 1300KB
     * - largest_psram ≥ 600KB
     */
    else if (free_psram >= (1300 * 1024) && largest_psram >= (600 * 1024)) {
        rb_size = 384 * 1024;
    }

    /* 
     * 强制打印每次启动的内存分档结果 
     */ 
    printf("[rb_tune] SDCard rb_size=%dKB, free_psram=%dKB, largest_psram=%dKB\n", 
           rb_size / 1024, free_psram / 1024, largest_psram / 1024); 
    return rb_size;
#else
    return RB_SIZE;
#endif
}

// Forward declaration for send_cmd
static esp_err_t send_cmd(sdcard_cmd_t cmd);

/*
 * Timer callback - lightweight, non-blocking
 * Only sends command to queue, actual work done in sdcard_cmd_task
 */
static void play_state_check_timer_callback(U16 timerId, void *arg)
{
    // Non-blocking: just send command to queue
    send_cmd(SDCARD_CMD_STATE_CHECK);
}

/*
 * 为 LOADING 阶段提供统一的状态检查重调度入口：
 * 1. 优先使用 FW 定时器，保持原有异步架构。
 * 2. 如果定时器启动失败，则退化为直接投递一次检查命令，避免卡死在 LOADING。
 */
static esp_err_t _schedule_loading_state_check(uint32_t delay_ms, const char *source)
{
    BaseType_t timer_ret;
    esp_err_t cmd_ret;

    if (s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    timer_ret = FW_SetTimer(play_state_check_timer_callback,
                            0,
                            (void *)SD_PLAYER_STATE_LOADING,
                            delay_ms);
    if (timer_ret == pdPASS) {
        return ESP_OK;
    }

    ESP_LOGW(TAG,
             "[loading_check] arm timer failed, src=%s delay=%lu, fallback direct cmd",
             source ? source : "unknown",
             (unsigned long)delay_ms);

    cmd_ret = send_cmd(SDCARD_CMD_STATE_CHECK);
    if (cmd_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "[loading_check] fallback cmd failed, src=%s ret=%d",
                 source ? source : "unknown",
                 (int)cmd_ret);
        return cmd_ret;
    }

    return ESP_OK;
}

/*
 * Actual implementation of state check
 * Called from sdcard_cmd_task, can safely perform blocking operations
 */
static void _play_state_check_impl()
{
    sdcard_state_t state = s_sdcard_handler->state;
    static uint8_t load_count = 0;
    if (s_sdcard_handler->resume_guard_active)
    {
        audio_element_state_t pipe_state = audio_pipeline_get_state(s_sdcard_handler->pipeline);
        audio_element_state_t fatfs_state = audio_element_get_state(s_sdcard_handler->element_fatfs);
        audio_element_state_t decoder_state = audio_element_get_state(s_sdcard_handler->element_decoder);
        audio_element_state_t filter_state = audio_element_get_state(s_sdcard_handler->element_rsp_filter);
        TickType_t resume_elapsed = xTaskGetTickCount() - s_sdcard_handler->resume_guard_enter_tick;

        if (state != SD_PLAYER_STATE_PAUSED ||
            pipe_state != AEL_STATE_RUNNING ||
            fatfs_state != AEL_STATE_RUNNING ||
            decoder_state != AEL_STATE_RUNNING ||
            filter_state != AEL_STATE_RUNNING)
        {
            s_sdcard_handler->resume_guard_active = false;
            ESP_LOGI(TAG, "[resume_guard] canceled state=%d pipe=%d fatfs=%d dec=%d filter=%d",
                     state, pipe_state, fatfs_state, decoder_state, filter_state);
            return;
        }

        if (resume_elapsed >= pdMS_TO_TICKS(SDCARD_STARTUP_RESUME_TIMEOUT_MS))
        {
            ESP_LOGW(TAG, "[resume_guard] timeout=%lu ms, keep paused and flush output",
                     (unsigned long)pdTICKS_TO_MS(resume_elapsed));
            _pause_pipeline_fast();
            set_state(SD_PLAYER_STATE_PAUSED);
            return;
        }

        s_sdcard_handler->startup_guard_active = true;
        if (_wait_track_startup_buffer_ready()) {
            return;
        }

        s_sdcard_handler->resume_guard_active = false;
        set_state(SD_PLAYER_STATE_PLAYING);
        ESP_LOGI(TAG, "[resume_guard] released, playback resumed at normal volume");
        return;
    }
    if (state == SD_PLAYER_STATE_SWITCHING && s_sdcard_handler->auto_next_pending)
    {
        (void)_handle_auto_next_wait();
        return;
    }
    if (state == SD_PLAYER_STATE_LOADING)
    {
        uint32_t loading_timeout_ms = _get_loading_timeout_ms();

        app_player_sdcard_print_state();
        if ((xTaskGetTickCount() - s_sdcard_handler->loading_enter_tick) >= pdMS_TO_TICKS(loading_timeout_ms))
        {
            load_count = 0;
            s_sdcard_handler->stats.loading_retry_escalate_count++;
            ESP_LOGW(TAG,
                     "[play_state_check] loading timeout=%lu ms, wav=%d, switch next",
                     (unsigned long)loading_timeout_ms,
                     s_sdcard_handler->current_track_is_wav ? 1 : 0);
            _log_diag_stats("loading_timeout");
            on_next_song_force();
            return;
        }
        if (audio_pipeline_get_state(s_sdcard_handler->pipeline) == AEL_STATE_RUNNING &&
            audio_element_get_state(s_sdcard_handler->element_fatfs) == AEL_STATE_RUNNING &&
            audio_element_get_state(s_sdcard_handler->element_decoder) == AEL_STATE_RUNNING &&
            audio_element_get_state(s_sdcard_handler->element_rsp_filter) == AEL_STATE_RUNNING)
        {
            mutex_lock(s_sdcard_handler->lock);
            if (s_sdcard_handler->current_playlist_url[0] == '\0')
            {
                ESP_LOGW(TAG, "[play_state_check] current url missing");
                mutex_unlock(s_sdcard_handler->lock);
                _play_latest();
            }
            else
            {
                displayer_track_num(_get_display_track_num_locked());

                mutex_unlock(s_sdcard_handler->lock);
                if (_wait_track_startup_buffer_ready()) {
                    return;
                }
                // stop timer
                set_state(SD_PLAYER_STATE_PLAYING);
            }
            load_count = 0;
        }
        else if (audio_pipeline_get_state(s_sdcard_handler->pipeline) > AEL_STATE_RUNNING ||
                 audio_element_get_state(s_sdcard_handler->element_fatfs) > AEL_STATE_RUNNING ||
                 audio_element_get_state(s_sdcard_handler->element_decoder) > AEL_STATE_RUNNING ||
                 audio_element_get_state(s_sdcard_handler->element_rsp_filter) > AEL_STATE_RUNNING)
        {
            mutex_lock(s_sdcard_handler->lock);
            ESP_LOGE(TAG, "[play_state_check] loading error, play:%s", s_sdcard_handler->current_playlist_url);
            s_sdcard_handler->stats.loading_retry_count++;
            mutex_unlock(s_sdcard_handler->lock);

            // 如果多次加载失败，则自动下一首
            load_count++;
            if (load_count > 3)
            {
                load_count = 0;
                s_sdcard_handler->stats.loading_retry_escalate_count++;
                ESP_LOGI(TAG, "[play_state_check_timer_callback] load retry exceed, switch next");
                _log_diag_stats("load_retry_exceed");
                on_next_song_force();
            }
            else
            {
                _play_latest();
            }
        }
        // int id = sdcard_list_get_url_id(s_sdcard_handler->sdcard_list );
        // displayer_track_num(id);
    }
}

static int _get_display_track_num_locked(void)
{
    if (s_sdcard_handler == NULL || s_sdcard_handler->track_total == 0U) {
        return 0;
    }
    return (int)(s_sdcard_handler->track_index + 1U);
}

int app_player_sdcard_get_display_track_num(void)
{
    int display_num = 0;

    if (s_sdcard_handler == NULL) {
        return 0;
    }

    mutex_lock(s_sdcard_handler->lock);
    display_num = _get_display_track_num_locked();
    mutex_unlock(s_sdcard_handler->lock);

    return display_num;
}

esp_err_t app_player_sdcard_refresh_track_display(void)
{
    int display_num = app_player_sdcard_get_display_track_num();

    if (display_num <= 0) {
        return ESP_ERR_INVALID_STATE;
    }

    displayer_track_num(display_num);
    return ESP_OK;
}

static uint16_t _read_le16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t _read_syncsafe_u32(const uint8_t *data)
{
    if (data == NULL) {
        return UINT32_MAX;
    }
    if ((data[0] & 0x80U) != 0U ||
        (data[1] & 0x80U) != 0U ||
        (data[2] & 0x80U) != 0U ||
        (data[3] & 0x80U) != 0U) {
        return UINT32_MAX;
    }
    return ((uint32_t)(data[0] & 0x7FU) << 21) |
           ((uint32_t)(data[1] & 0x7FU) << 14) |
           ((uint32_t)(data[2] & 0x7FU) << 7) |
           (uint32_t)(data[3] & 0x7FU);
}

static void _write_le16(uint8_t *data, uint16_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void _record_mp3_check_fail(const char *reason, const char *url)
{
    if (s_sdcard_handler == NULL) {
        return;
    }

    s_sdcard_handler->stats.mp3_check_fail_count++;
    if (reason != NULL) {
        snprintf(s_sdcard_handler->stats.last_mp3_fail_reason,
                 sizeof(s_sdcard_handler->stats.last_mp3_fail_reason),
                 "%s", reason);
    } else {
        s_sdcard_handler->stats.last_mp3_fail_reason[0] = '\0';
    }

    if (url != NULL) {
        snprintf(s_sdcard_handler->stats.last_mp3_fail_url,
                 sizeof(s_sdcard_handler->stats.last_mp3_fail_url),
                 "%s", url);
    } else {
        s_sdcard_handler->stats.last_mp3_fail_url[0] = '\0';
    }

    ESP_LOGW(TAG, "[mp3_diag] fail reason=%s url=%s",
             s_sdcard_handler->stats.last_mp3_fail_reason,
             s_sdcard_handler->stats.last_mp3_fail_url);
}

static void _record_wav_check_fail(const char *reason, const char *url)
{
    if (s_sdcard_handler == NULL) {
        return;
    }

    s_sdcard_handler->stats.wav_check_fail_count++;
    if (reason != NULL) {
        snprintf(s_sdcard_handler->stats.last_wav_fail_reason,
                 sizeof(s_sdcard_handler->stats.last_wav_fail_reason),
                 "%s", reason);
    } else {
        s_sdcard_handler->stats.last_wav_fail_reason[0] = '\0';
    }

    if (url != NULL) {
        snprintf(s_sdcard_handler->stats.last_wav_fail_url,
                 sizeof(s_sdcard_handler->stats.last_wav_fail_url),
                 "%s", url);
    } else {
        s_sdcard_handler->stats.last_wav_fail_url[0] = '\0';
    }

    ESP_LOGW(TAG, "[wav_diag] fail reason=%s url=%s",
             s_sdcard_handler->stats.last_wav_fail_reason,
             s_sdcard_handler->stats.last_wav_fail_url);
}

static void _log_diag_stats(const char *stage)
{
    if (s_sdcard_handler == NULL) {
        return;
    }

    ESP_LOGI(TAG,
             "[diag][%s] mp3_check=%lu mp3_fail=%lu mp3_skip=%lu wav_check=%lu wav_fail=%lu wav_skip=%lu next_req=%lu next_drop=%lu prev_req=%lu prev_drop=%lu load_retry=%lu load_escalate=%lu dec_err=%lu last_mp3_reason=%s last_mp3_url=%s last_wav_reason=%s last_wav_url=%s",
             stage ? stage : "null",
             (unsigned long)s_sdcard_handler->stats.mp3_check_count,
             (unsigned long)s_sdcard_handler->stats.mp3_check_fail_count,
             (unsigned long)s_sdcard_handler->stats.mp3_skip_count,
             (unsigned long)s_sdcard_handler->stats.wav_check_count,
             (unsigned long)s_sdcard_handler->stats.wav_check_fail_count,
             (unsigned long)s_sdcard_handler->stats.wav_skip_count,
             (unsigned long)s_sdcard_handler->stats.next_request_count,
             (unsigned long)s_sdcard_handler->stats.next_drop_count,
             (unsigned long)s_sdcard_handler->stats.prev_request_count,
             (unsigned long)s_sdcard_handler->stats.prev_drop_count,
             (unsigned long)s_sdcard_handler->stats.loading_retry_count,
             (unsigned long)s_sdcard_handler->stats.loading_retry_escalate_count,
             (unsigned long)s_sdcard_handler->stats.decoder_error_count,
             s_sdcard_handler->stats.last_mp3_fail_reason,
             s_sdcard_handler->stats.last_mp3_fail_url,
             s_sdcard_handler->stats.last_wav_fail_reason,
             s_sdcard_handler->stats.last_wav_fail_url);
}

static uint32_t _read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static void _write_le32(uint8_t *data, uint32_t value)
{
    data[0] = (uint8_t)(value & 0xFFU);
    data[1] = (uint8_t)((value >> 8) & 0xFFU);
    data[2] = (uint8_t)((value >> 16) & 0xFFU);
    data[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static bool _char_equal_ignore_case(char lhs, char rhs)
{
    if (lhs >= 'A' && lhs <= 'Z') {
        lhs = (char)(lhs - 'A' + 'a');
    }
    if (rhs >= 'A' && rhs <= 'Z') {
        rhs = (char)(rhs - 'A' + 'a');
    }
    return lhs == rhs;
}

static bool _ends_with_ignore_case(const char *text, const char *suffix)
{
    size_t text_len = 0;
    size_t suffix_len = 0;
    size_t i = 0;

    if (text == NULL || suffix == NULL) {
        return false;
    }

    text_len = strlen(text);
    suffix_len = strlen(suffix);
    if (text_len < suffix_len) {
        return false;
    }

    for (i = 0; i < suffix_len; i++) {
        if (!_char_equal_ignore_case(text[text_len - suffix_len + i], suffix[i])) {
            return false;
        }
    }
    return true;
}

static bool _is_wav_url(const char *url)
{
    return _ends_with_ignore_case(url, ".wav");
}

static bool _is_mp3_url(const char *url)
{
    return _ends_with_ignore_case(url, ".mp3");
}

static track_format_t _classify_track_format(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return TRACK_FORMAT_UNKNOWN;
    }
    if (_is_mp3_url(url)) {
        return TRACK_FORMAT_MP3;
    }
    if (_is_wav_url(url)) {
        return TRACK_FORMAT_WAV;
    }
    return TRACK_FORMAT_OTHER;
}

static esp_err_t _ensure_cache_fs_ready(void)
{
    esp_err_t ret = ESP_OK;
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = SDCARD_CACHE_BASE_PATH,
        .partition_label = SDCARD_CACHE_PARTITION_LABEL,
        .max_files = 6,
        .format_if_mount_failed = true,
    };

    if (s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_sdcard_handler->cache_fs_ready) {
        return ESP_OK;
    }

    ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[cache_fs] mount failed: %s", esp_err_to_name(ret));
        return ESP_FAIL;
    }

    s_sdcard_handler->cache_fs_ready = true;
    if (esp_spiffs_info(SDCARD_CACHE_PARTITION_LABEL, &total_bytes, &used_bytes) == ESP_OK) {
        ESP_LOGI(TAG,
                 "[cache_fs] mounted at %s total=%u used=%u",
                 SDCARD_CACHE_BASE_PATH,
                 (unsigned int)total_bytes,
                 (unsigned int)used_bytes);
    } else {
        ESP_LOGI(TAG, "[cache_fs] mounted at %s", SDCARD_CACHE_BASE_PATH);
    }
    return ESP_OK;
}

static uint32_t _fnv1a_update(uint32_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t index = 0;

    if (bytes == NULL) {
        return hash;
    }

    for (index = 0; index < len; index++) {
        hash ^= bytes[index];
        hash *= 16777619UL;
    }
    return hash;
}

static uint32_t _hash_text(const char *text)
{
    uint32_t hash = 2166136261UL;

    if (text == NULL) {
        return 0U;
    }

    return _fnv1a_update(hash, text, strlen(text));
}

static void _reset_root_fingerprint(sdcard_root_fingerprint_t *fingerprint)
{
    if (fingerprint == NULL) {
        return;
    }

    memset(fingerprint, 0, sizeof(*fingerprint));
    fingerprint->signature = 2166136261UL;
}

static void _update_root_fingerprint(sdcard_root_fingerprint_t *fingerprint,
                                     const char *base_name,
                                     const struct stat *st)
{
    uint32_t size_u32 = 0;
    uint32_t mtime_u32 = 0;

    if (fingerprint == NULL || base_name == NULL || st == NULL || st->st_size <= 0) {
        return;
    }

    size_u32 = (uint32_t)st->st_size;
    if (st->st_mtime > 0) {
        mtime_u32 = (uint32_t)st->st_mtime;
    }

    fingerprint->file_count++;
    fingerprint->total_size += (uint64_t)size_u32;
    if (mtime_u32 > fingerprint->latest_mtime) {
        fingerprint->latest_mtime = mtime_u32;
    }

    fingerprint->signature = _fnv1a_update(fingerprint->signature, base_name, strlen(base_name));
    fingerprint->signature = _fnv1a_update(fingerprint->signature, &size_u32, sizeof(size_u32));
    fingerprint->signature = _fnv1a_update(fingerprint->signature, &mtime_u32, sizeof(mtime_u32));
}

static uint32_t _get_loading_timeout_ms(void)
{
    return SDCARD_LOADING_TIMEOUT_MS;
}

static const char *_path_basename(const char *path)
{
    const char *name = path;
    const char *cursor = NULL;

    if (path == NULL) {
        return NULL;
    }

    for (cursor = path; *cursor != '\0'; cursor++) {
        if (*cursor == '/' || *cursor == '\\') {
            name = cursor + 1;
        }
    }
    return name;
}

static bool _is_scan_hidden_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return true;
    }

    if (name[0] == '.') {
        return true;
    }

    if (_ends_with_ignore_case(name, ".ini") ||
        _ends_with_ignore_case(name, ".lnk") ||
        _ends_with_ignore_case(name, ".db")) {
        return true;
    }

    if (_ends_with_ignore_case(name, ".DS_Store") ||
        _ends_with_ignore_case(name, "Thumbs.db") ||
        _ends_with_ignore_case(name, "desktop.ini")) {
        return true;
    }

    return false;
}

static bool _has_invalid_filename_char(const char *name)
{
    static const char invalid_chars[] = ":*?\"<>|";
    const char *cursor = NULL;

    if (name == NULL) {
        return true;
    }

    for (cursor = name; *cursor != '\0'; cursor++) {
        if (strchr(invalid_chars, *cursor) != NULL) {
            return true;
        }
    }
    return false;
}

static bool _is_scan_temp_name(const char *name)
{
    if (name == NULL) {
        return true;
    }

    return _ends_with_ignore_case(name, ".tmp") ||
           _ends_with_ignore_case(name, ".part") ||
           _ends_with_ignore_case(name, ".partial") ||
           _ends_with_ignore_case(name, ".crdownload") ||
           _ends_with_ignore_case(name, ".download");
}

static bool _is_supported_audio_file(const char *name)
{
    if (name == NULL) {
        return false;
    }

    return _ends_with_ignore_case(name, ".mp3") ||
           _ends_with_ignore_case(name, ".m4a") ||
           _ends_with_ignore_case(name, ".flac") ||
           _ends_with_ignore_case(name, ".ogg") ||
           _ends_with_ignore_case(name, ".opus") ||
           _ends_with_ignore_case(name, ".amr") ||
           _ends_with_ignore_case(name, ".ts") ||
           _ends_with_ignore_case(name, ".aac") ||
           _ends_with_ignore_case(name, ".wav");
}

static bool _is_duplicate_hash(const uint32_t *list, uint32_t count, uint32_t hash)
{
    uint32_t index = 0;

    if (list == NULL || hash == 0U) {
        return false;
    }

    for (index = 0; index < count; index++) {
        if (list[index] == hash) {
            return true;
        }
    }
    return false;
}

/*
 * 歌曲固定存放在根目录，这里只做根目录轻量遍历，不打开音频文件。
 */
static esp_err_t _collect_root_fingerprint(const char *dir_path, sdcard_root_fingerprint_t *fingerprint)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    if (dir_path == NULL || fingerprint == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    _reset_root_fingerprint(fingerprint);
    dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "[cache] open root dir failed: %s", dir_path);
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        char full_path[SDCARD_ACTIVE_URL_MAX_LEN];
        const char *name = entry->d_name;

        if (name == NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }
        if (_is_scan_hidden_name(name) ||
            _is_scan_temp_name(name) ||
            !_is_supported_audio_file(name) ||
            _has_invalid_filename_char(name)) {
            continue;
        }
        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name) >= (int)sizeof(full_path)) {
            ESP_LOGW(TAG, "[cache] skip long root path: %s/%s", dir_path, name);
            continue;
        }
        if (stat(full_path, &st) != 0) {
            ESP_LOGW(TAG, "[cache] root stat failed: %s", full_path);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }
        if (st.st_size <= 0 || (uint32_t)st.st_size > SDCARD_MAX_AUDIO_FILE_SIZE_BYTES) {
            continue;
        }

        _update_root_fingerprint(fingerprint, name, &st);
    }

    closedir(dir);
    return ESP_OK;
}

static bool _is_root_fingerprint_match(const sdcard_cache_idx_header_t *header,
                                       const sdcard_root_fingerprint_t *fingerprint)
{
    if (header == NULL || fingerprint == NULL) {
        return false;
    }

    return header->root_file_count == fingerprint->file_count &&
           header->root_latest_mtime == fingerprint->latest_mtime &&
           header->root_total_size == fingerprint->total_size &&
           header->root_signature == fingerprint->signature;
}

static esp_err_t _load_track_cache_from_spiffs(void)
{
    sdcard_cache_idx_header_t header;
    track_play_plan_t plan;

    if (s_sdcard_handler == NULL || s_sdcard_handler->element_fatfs == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (_cache_read_header(&header) != ESP_OK || header.track_count == 0U) {
        return ESP_FAIL;
    }

    remove(SDCARD_WAV_SHADOW_PATH);
    _clear_current_track_context();
    s_sdcard_handler->track_total = header.track_count;
    s_sdcard_handler->track_scan_signature = header.scan_signature;
    if (_cache_set_current_index(0U) != ESP_OK || _select_playable_track(&plan) != ESP_OK) {
        _clear_current_track_context();
        return ESP_FAIL;
    }

    _remember_current_track(&plan);
    audio_element_set_uri(s_sdcard_handler->element_fatfs, plan.playback_url);
    app_player_sdcard_refresh_track_display();
    ESP_LOGI(TAG,
             "[cache] load cached playlist tracks=%lu signature=%lu",
             (unsigned long)s_sdcard_handler->track_total,
             (unsigned long)s_sdcard_handler->track_scan_signature);
    return ESP_OK;
}

static esp_err_t _restore_cached_tracks(bool *out_need_rebuild)
{
    sdcard_cache_idx_header_t header;
    sdcard_root_fingerprint_t root_fp;
    bool need_rebuild = true;

    if (out_need_rebuild != NULL) {
        *out_need_rebuild = true;
    }
    if (_cache_read_header(&header) != ESP_OK || header.track_count == 0U) {
        return ESP_FAIL;
    }

    if (_collect_root_fingerprint("/sdcard", &root_fp) == ESP_OK &&
        _is_root_fingerprint_match(&header, &root_fp)) {
        need_rebuild = false;
        ESP_LOGI(TAG,
                 "[cache] root fingerprint match count=%lu latest=%lu signature=%lu",
                 (unsigned long)root_fp.file_count,
                 (unsigned long)root_fp.latest_mtime,
                 (unsigned long)root_fp.signature);
    } else {
        ESP_LOGW(TAG,
                 "[cache] root fingerprint changed old=%lu/%lu/%lu new=%lu/%lu/%lu",
                 (unsigned long)header.root_file_count,
                 (unsigned long)header.root_latest_mtime,
                 (unsigned long)header.root_signature,
                 (unsigned long)root_fp.file_count,
                 (unsigned long)root_fp.latest_mtime,
                 (unsigned long)root_fp.signature);
    }

    if (_load_track_cache_from_spiffs() != ESP_OK) {
        return ESP_FAIL;
    }

    if (out_need_rebuild != NULL) {
        *out_need_rebuild = need_rebuild;
    }
    return ESP_OK;
}

/*
 * 启动时优先恢复旧缓存，只有检测到根目录发生变化时才在后台重建。
 */
static esp_err_t _prepare_track_cache_on_startup(void)
{
    bool need_rebuild = true;
    esp_err_t ret = ESP_OK;

    if (s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = _restore_cached_tracks(&need_rebuild);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "[cache] startup restore ok tracks=%lu rebuild=%d",
                 (unsigned long)s_sdcard_handler->track_total,
                 need_rebuild ? 1 : 0);
        if (!need_rebuild) {
            return ESP_OK;
        }

        ret = _trigger_sdcard_scan_task(false);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "[cache] background rebuild start failed");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "[cache] use cached playlist and rebuild in background");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "[cache] restore failed, trigger full scan");
    return _trigger_sdcard_scan_task(true);
}

static esp_err_t _cache_read_header(sdcard_cache_idx_header_t *header)
{
    FILE *fp = NULL;

    if (header == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (_ensure_cache_fs_ready() != ESP_OK) {
        return ESP_FAIL;
    }

    fp = fopen(SDCARD_CACHE_IDX_PATH, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    if (fread(header, 1, sizeof(*header), fp) != sizeof(*header)) {
        fclose(fp);
        return ESP_FAIL;
    }
    fclose(fp);

    if (memcmp(header->magic, SDCARD_CACHE_IDX_MAGIC, sizeof(header->magic) - 1) != 0 ||
        header->version != SDCARD_CACHE_IDX_VERSION) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t _cache_read_track_url_by_index(uint32_t index, char *url, size_t url_size)
{
    sdcard_cache_idx_header_t header;
    FILE *idx_fp = NULL;
    FILE *m3u_fp = NULL;
    uint32_t offset = 0;
    size_t read_len = 0;

    if (url == NULL || url_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    url[0] = '\0';

    if (_cache_read_header(&header) != ESP_OK) {
        return ESP_FAIL;
    }
    if (index >= header.track_count) {
        return ESP_ERR_INVALID_ARG;
    }

    idx_fp = fopen(SDCARD_CACHE_IDX_PATH, "rb");
    if (idx_fp == NULL) {
        return ESP_FAIL;
    }

    if (fseek(idx_fp, (long)(sizeof(header) + (index * sizeof(uint32_t))), SEEK_SET) != 0 ||
        fread(&offset, 1, sizeof(offset), idx_fp) != sizeof(offset)) {
        fclose(idx_fp);
        return ESP_FAIL;
    }
    fclose(idx_fp);

    m3u_fp = fopen(SDCARD_CACHE_M3U_PATH, "rb");
    if (m3u_fp == NULL) {
        return ESP_FAIL;
    }

    if (fseek(m3u_fp, (long)offset, SEEK_SET) != 0 ||
        fgets(url, (int)url_size, m3u_fp) == NULL) {
        fclose(m3u_fp);
        url[0] = '\0';
        return ESP_FAIL;
    }
    fclose(m3u_fp);

    read_len = strlen(url);
    while (read_len > 0U &&
           (url[read_len - 1U] == '\r' || url[read_len - 1U] == '\n')) {
        url[read_len - 1U] = '\0';
        read_len--;
    }

    return (url[0] != '\0') ? ESP_OK : ESP_FAIL;
}

static esp_err_t _cache_refresh_neighbors(void)
{
    uint32_t prev_index = 0;
    uint32_t next_index = 0;

    if (s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_sdcard_handler->prev_playlist_url[0] = '\0';
    s_sdcard_handler->current_playlist_url[0] = '\0';
    s_sdcard_handler->next_playlist_url[0] = '\0';

    if (s_sdcard_handler->track_total == 0U) {
        return ESP_FAIL;
    }

    /* 上一曲不回绕到最后一首，clamp 到 index 0，防止歌曲过多时回绕出错 */
    prev_index = (s_sdcard_handler->track_index == 0U) ?
                 0U :
                 (s_sdcard_handler->track_index - 1U);
    next_index = (s_sdcard_handler->track_index + 1U) % s_sdcard_handler->track_total;

    if (_cache_read_track_url_by_index(prev_index,
                                       s_sdcard_handler->prev_playlist_url,
                                       sizeof(s_sdcard_handler->prev_playlist_url)) != ESP_OK ||
        _cache_read_track_url_by_index(s_sdcard_handler->track_index,
                                       s_sdcard_handler->current_playlist_url,
                                       sizeof(s_sdcard_handler->current_playlist_url)) != ESP_OK ||
        _cache_read_track_url_by_index(next_index,
                                       s_sdcard_handler->next_playlist_url,
                                       sizeof(s_sdcard_handler->next_playlist_url)) != ESP_OK) {
        s_sdcard_handler->prev_playlist_url[0] = '\0';
        s_sdcard_handler->current_playlist_url[0] = '\0';
        s_sdcard_handler->next_playlist_url[0] = '\0';
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t _cache_set_current_index(uint32_t index)
{
    if (s_sdcard_handler == NULL || s_sdcard_handler->track_total == 0U) {
        return ESP_ERR_INVALID_STATE;
    }

    if (index >= s_sdcard_handler->track_total) {
        index %= s_sdcard_handler->track_total;
    }

    s_sdcard_handler->track_index = index;
    return _cache_refresh_neighbors();
}

static esp_err_t _find_track_index_in_cache_m3u(const char *m3u_path, const char *target_url, uint32_t *out_index)
{
    FILE *fp = NULL;
    char line[SDCARD_ACTIVE_URL_MAX_LEN];
    uint32_t index = 0;

    if (m3u_path == NULL || target_url == NULL || target_url[0] == '\0' || out_index == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fp = fopen(m3u_path, "rb");
    if (fp == NULL) {
        return ESP_FAIL;
    }

    while (fgets(line, (int)sizeof(line), fp) != NULL) {
        size_t read_len = strlen(line);

        while (read_len > 0U &&
               (line[read_len - 1U] == '\r' || line[read_len - 1U] == '\n')) {
            line[read_len - 1U] = '\0';
            read_len--;
        }

        if (strcmp(line, target_url) == 0) {
            fclose(fp);
            *out_index = index;
            return ESP_OK;
        }
        index++;
    }

    fclose(fp);
    return ESP_FAIL;
}

/*
 * 后台扫描只生成 stage 缓存，真正切换到新缓存放到切歌路径里完成，避免扫描线程改动播放链路。
 */
static esp_err_t _apply_pending_cache_if_needed(void)
{
    sdcard_cache_idx_header_t header;
    uint32_t target_index = 0U;
    uint32_t old_index = 0U;
    char preferred_url[SDCARD_ACTIVE_URL_MAX_LEN];

    if (s_sdcard_handler == NULL || !s_sdcard_handler->cache_swap_pending) {
        return ESP_OK;
    }

    preferred_url[0] = '\0';
    old_index = s_sdcard_handler->track_index;
    if (s_sdcard_handler->current_playlist_url[0] != '\0') {
        snprintf(preferred_url, sizeof(preferred_url), "%s", s_sdcard_handler->current_playlist_url);
    } else if (s_sdcard_handler->current_playback_url[0] != '\0' &&
               strcmp(s_sdcard_handler->current_playback_url, SDCARD_WAV_SHADOW_PATH) != 0) {
        snprintf(preferred_url, sizeof(preferred_url), "%s", s_sdcard_handler->current_playback_url);
    }

    if (remove(SDCARD_CACHE_M3U_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "[cache] remove active m3u failed");
        return ESP_FAIL;
    }
    if (remove(SDCARD_CACHE_IDX_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "[cache] remove active idx failed");
        return ESP_FAIL;
    }
    if (rename(SDCARD_CACHE_M3U_STAGE_PATH, SDCARD_CACHE_M3U_PATH) != 0 ||
        rename(SDCARD_CACHE_IDX_STAGE_PATH, SDCARD_CACHE_IDX_PATH) != 0) {
        ESP_LOGW(TAG, "[cache] activate stage cache failed");
        return ESP_FAIL;
    }

    if (_cache_read_header(&header) != ESP_OK || header.track_count == 0U) {
        ESP_LOGW(TAG, "[cache] active stage header invalid");
        return ESP_FAIL;
    }

    s_sdcard_handler->track_total = header.track_count;
    s_sdcard_handler->track_scan_signature = header.scan_signature;
    s_sdcard_handler->cache_swap_pending = false;

    if (preferred_url[0] != '\0' &&
        _find_track_index_in_cache_m3u(SDCARD_CACHE_M3U_PATH, preferred_url, &target_index) == ESP_OK) {
        ESP_LOGI(TAG, "[cache] activate stage by current url index=%lu", (unsigned long)target_index);
    } else if (header.track_count > 0U) {
        target_index = old_index % header.track_count;
        ESP_LOGI(TAG, "[cache] activate stage by fallback index=%lu", (unsigned long)target_index);
    }

    return _cache_set_current_index(target_index);
}

/*
 * 扫描阶段按格式分流到不同临时文件，避免后续再用 DRAM 整表重建播放列表。
 */
static FILE *_select_scan_bucket_file(sdcard_scan_context_t *ctx, const char *path, track_format_t *out_format)
{
    track_format_t format = TRACK_FORMAT_OTHER;

    if (ctx == NULL) {
        return NULL;
    }

    format = _classify_track_format(path);
    if (out_format != NULL) {
        *out_format = format;
    }

    switch (format)
    {
    case TRACK_FORMAT_MP3:
        return ctx->mp3_fp;
    case TRACK_FORMAT_WAV:
        return ctx->wav_fp;
    case TRACK_FORMAT_OTHER:
    case TRACK_FORMAT_UNKNOWN:
    default:
        return ctx->other_fp;
    }
}

/*
 * 将分类后的临时文件合并到最终缓存，按写入顺序同步生成 m3u 和 idx。
 */
static esp_err_t _merge_scan_bucket_to_cache(FILE *src_fp, sdcard_scan_context_t *ctx, uint32_t *merged_count, const char *skip_url)
{
    char line[SDCARD_ACTIVE_URL_MAX_LEN + 48];

    if (src_fp == NULL || ctx == NULL || ctx->m3u_fp == NULL || ctx->idx_fp == NULL || merged_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (fseek(src_fp, 0L, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    while (fgets(line, (int)sizeof(line), src_fp) != NULL) {
        size_t read_len = strlen(line);
        uint32_t line_offset = 0;
        uint32_t size_u32 = 0;
        uint32_t mtime_u32 = 0;
        char *mtime_sep = NULL;
        char *size_sep = NULL;
        char *size_text = NULL;
        char *mtime_text = NULL;

        while (read_len > 0U &&
               (line[read_len - 1U] == '\r' || line[read_len - 1U] == '\n')) {
            line[read_len - 1U] = '\0';
            read_len--;
        }

        if (line[0] == '\0') {
            continue;
        }

        mtime_sep = strrchr(line, '|');
        if (mtime_sep == NULL) {
            ESP_LOGW(TAG, "[scan] invalid cache temp record");
            return ESP_FAIL;
        }
        *mtime_sep = '\0';
        mtime_text = mtime_sep + 1;

        size_sep = strrchr(line, '|');
        if (size_sep == NULL) {
            ESP_LOGW(TAG, "[scan] invalid cache temp record");
            return ESP_FAIL;
        }
        *size_sep = '\0';
        size_text = size_sep + 1;

        if (line[0] == '\0' || size_text[0] == '\0' || mtime_text[0] == '\0') {
            ESP_LOGW(TAG, "[scan] empty cache temp record");
            return ESP_FAIL;
        }

        /* 若当前合并行与需跳过的 url 相同（说明该歌已强制摆在播放列表首位了），则直接跳过 */
        if (skip_url != NULL && strcmp(line, skip_url) == 0) {
            continue;
        }

        size_u32 = (uint32_t)strtoul(size_text, NULL, 10);
        mtime_u32 = (uint32_t)strtoul(mtime_text, NULL, 10);
        if (size_u32 == 0U) {
            ESP_LOGW(TAG, "[scan] invalid cache temp size");
            return ESP_FAIL;
        }

        read_len = strlen(line);

        line_offset = (uint32_t)ftell(ctx->m3u_fp);
        if (fprintf(ctx->m3u_fp, "%s\n", line) < 0 ||
            fwrite(&line_offset, 1, sizeof(line_offset), ctx->idx_fp) != sizeof(line_offset)) {
            ESP_LOGE(TAG, "[scan] merge cache failed");
            return ESP_FAIL;
        }

        ctx->signature = _fnv1a_update(ctx->signature, line, read_len);
        ctx->signature = _fnv1a_update(ctx->signature, &size_u32, sizeof(size_u32));
        ctx->signature = _fnv1a_update(ctx->signature, &mtime_u32, sizeof(mtime_u32));
        (*merged_count)++;
    }

    return ESP_OK;
}

/*
 * 业务已约束歌曲只放根目录，因此扫描时忽略所有子目录，避免递归放大耗时。
 */
static esp_err_t _scan_directory_to_cache(const char *dir_path, sdcard_scan_context_t *ctx)
{
    DIR *dir = NULL;
    struct dirent *entry = NULL;

    if (dir_path == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->count >= ctx->limit) {
        return ESP_OK;
    }

    dir = opendir(dir_path);
    if (dir == NULL) {
        ESP_LOGW(TAG, "[scan] open dir failed: %s", dir_path);
        return ESP_FAIL;
    }

    while ((entry = readdir(dir)) != NULL && ctx->count < ctx->limit) {
        struct stat st;
        char full_path[SDCARD_ACTIVE_URL_MAX_LEN];
        const char *name = entry->d_name;

        if (name == NULL || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        if (snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, name) >= (int)sizeof(full_path)) {
            ESP_LOGW(TAG, "[scan] skip long path: %s/%s", dir_path, name);
            continue;
        }

        if (stat(full_path, &st) != 0) {
            ESP_LOGW(TAG, "[scan] stat failed: %s", full_path);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            continue;
        }

        if (S_ISREG(st.st_mode)) {
            const char *base_name = _path_basename(full_path);
            size_t base_name_len = 0;
            uint32_t path_hash = 0;
            uint32_t name_hash = 0;
            FILE *bucket_fp = NULL;
            track_format_t format = TRACK_FORMAT_OTHER;

            if (_is_scan_hidden_name(base_name) ||
                _is_scan_temp_name(base_name) ||
                !_is_supported_audio_file(base_name) ||
                _has_invalid_filename_char(base_name)) {
                continue;
            }

            /* 文件名字节数过长直接跳过，避免超长名称进入播放列表缓存。 */
            base_name_len = strlen(base_name);
            if (base_name_len > SDCARD_MAX_FILENAME_BYTES) {
                ESP_LOGW(TAG, "[scan] skip long filename(%u): %s",
                         (unsigned int)base_name_len,
                         full_path);
                continue;
            }

            if (st.st_size <= 0) {
                ESP_LOGW(TAG, "[scan] skip zero size file: %s", full_path);
                continue;
            }
            if ((uint32_t)st.st_size > SDCARD_MAX_AUDIO_FILE_SIZE_BYTES) {
                ESP_LOGW(TAG, "[scan] skip oversized file: %s", full_path);
                continue;
            }

            path_hash = _hash_text(full_path);
            name_hash = _hash_text(base_name);
            if (_is_duplicate_hash(ctx->path_hashes, ctx->count, path_hash) ||
                _is_duplicate_hash(ctx->name_hashes, ctx->count, name_hash)) {
                ESP_LOGW(TAG, "[scan] skip duplicate file: %s", full_path);
                continue;
            }

            bucket_fp = _select_scan_bucket_file(ctx, full_path, &format);
            if (bucket_fp == NULL) {
                closedir(dir);
                ESP_LOGE(TAG, "[scan] bucket file missing");
                return ESP_FAIL;
            }

            if (fprintf(bucket_fp, "%s|%lu|%lu\n",
                        full_path,
                        (unsigned long)((uint32_t)st.st_size),
                        (unsigned long)((st.st_mtime > 0) ? (uint32_t)st.st_mtime : 0U)) < 0) {
                closedir(dir);
                ESP_LOGE(TAG, "[scan] write cache failed");
                return ESP_FAIL;
            }

            _update_root_fingerprint(&ctx->root_fp, base_name, &st);
            ctx->path_hashes[ctx->count] = path_hash;
            ctx->name_hashes[ctx->count] = name_hash;
            ctx->count++;

            switch (format)
            {
            case TRACK_FORMAT_MP3:
                ctx->mp3_count++;
                break;
            case TRACK_FORMAT_WAV:
                ctx->wav_count++;
                break;
            case TRACK_FORMAT_OTHER:
            case TRACK_FORMAT_UNKNOWN:
            default:
                ctx->other_count++;
                break;
            }

            /* 增量模式：每扫到一首立即追加到 active M3U/IDX */
            if (ctx->incremental_mode && ctx->active_m3u_fp != NULL && ctx->active_idx_fp != NULL) {
                if (_incremental_append_to_active_cache(ctx, full_path) != ESP_OK) {
                    ESP_LOGW(TAG, "[scan] incremental append failed: %s", full_path);
                }
            }
        }
    }

    closedir(dir);
    return ESP_OK;
}

static esp_err_t _rebuild_track_cache(bool activate_now)
{
    sdcard_scan_context_t ctx;
    sdcard_cache_idx_header_t header;
    sdcard_cache_idx_header_t old_header;
    uint32_t merged_count = 0;
    esp_err_t ret = ESP_OK;
    bool cache_changed = true;

    if (s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (_ensure_cache_fs_ready() != ESP_OK) {
        return ESP_FAIL;
    }

    memset(&ctx, 0, sizeof(ctx));
    memset(&header, 0, sizeof(header));
    memset(&old_header, 0, sizeof(old_header));
    ctx.limit = SDCARD_SCAN_MAX_TRACKS;
    ctx.signature = 2166136261UL;
    _reset_root_fingerprint(&ctx.root_fp);

    remove(SDCARD_CACHE_M3U_TMP_PATH);
    remove(SDCARD_CACHE_IDX_TMP_PATH);
    remove(SDCARD_CACHE_M3U_MP3_TMP_PATH);
    remove(SDCARD_CACHE_M3U_WAV_TMP_PATH);
    remove(SDCARD_CACHE_M3U_OTHER_TMP_PATH);

    ctx.m3u_fp = fopen(SDCARD_CACHE_M3U_TMP_PATH, "wb");
    ctx.idx_fp = fopen(SDCARD_CACHE_IDX_TMP_PATH, "wb+");
    ctx.mp3_fp = fopen(SDCARD_CACHE_M3U_MP3_TMP_PATH, "wb+");
    ctx.wav_fp = fopen(SDCARD_CACHE_M3U_WAV_TMP_PATH, "wb+");
    ctx.other_fp = fopen(SDCARD_CACHE_M3U_OTHER_TMP_PATH, "wb+");
    if (ctx.m3u_fp == NULL || ctx.idx_fp == NULL ||
        ctx.mp3_fp == NULL || ctx.wav_fp == NULL || ctx.other_fp == NULL) {
        if (ctx.m3u_fp != NULL) {
            fclose(ctx.m3u_fp);
        }
        if (ctx.idx_fp != NULL) {
            fclose(ctx.idx_fp);
        }
        if (ctx.mp3_fp != NULL) {
            fclose(ctx.mp3_fp);
        }
        if (ctx.wav_fp != NULL) {
            fclose(ctx.wav_fp);
        }
        if (ctx.other_fp != NULL) {
            fclose(ctx.other_fp);
        }
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        remove(SDCARD_CACHE_M3U_MP3_TMP_PATH);
        remove(SDCARD_CACHE_M3U_WAV_TMP_PATH);
        remove(SDCARD_CACHE_M3U_OTHER_TMP_PATH);
        ESP_LOGE(TAG, "[scan] create cache temp file failed");
        return ESP_FAIL;
    }

    memset(&header, 0, sizeof(header));
    fwrite(&header, 1, sizeof(header), ctx.idx_fp);

    ret = _scan_directory_to_cache("/sdcard", &ctx);
    if (ret == ESP_OK) {
        if (_merge_scan_bucket_to_cache(ctx.mp3_fp, &ctx, &merged_count, NULL) != ESP_OK ||
            _merge_scan_bucket_to_cache(ctx.wav_fp, &ctx, &merged_count, NULL) != ESP_OK ||
            _merge_scan_bucket_to_cache(ctx.other_fp, &ctx, &merged_count, NULL) != ESP_OK) {
            ret = ESP_FAIL;
        }
    }
    fflush(ctx.m3u_fp);
    fflush(ctx.idx_fp);

    if (ret == ESP_OK && merged_count == 0U) {
        ESP_LOGW(TAG, "[scan] no valid tracks found");
        ret = ESP_FAIL;
    }

    if (ret == ESP_OK) {
        memset(&header, 0, sizeof(header));
        memcpy(header.magic, SDCARD_CACHE_IDX_MAGIC, sizeof(header.magic) - 1U);
        header.version = SDCARD_CACHE_IDX_VERSION;
        header.track_count = merged_count;
        header.scan_signature = ctx.signature;
        header.root_file_count = ctx.root_fp.file_count;
        header.root_latest_mtime = ctx.root_fp.latest_mtime;
        header.root_total_size = ctx.root_fp.total_size;
        header.root_signature = ctx.root_fp.signature;
        if (fseek(ctx.idx_fp, 0L, SEEK_SET) != 0 ||
            fwrite(&header, 1, sizeof(header), ctx.idx_fp) != sizeof(header)) {
            ret = ESP_FAIL;
        }
    }

    fclose(ctx.m3u_fp);
    fclose(ctx.idx_fp);
    fclose(ctx.mp3_fp);
    fclose(ctx.wav_fp);
    fclose(ctx.other_fp);
    remove(SDCARD_CACHE_M3U_MP3_TMP_PATH);
    remove(SDCARD_CACHE_M3U_WAV_TMP_PATH);
    remove(SDCARD_CACHE_M3U_OTHER_TMP_PATH);

    if (ret != ESP_OK) {
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        return ret;
    }

    if (_cache_read_header(&old_header) == ESP_OK) {
        if (old_header.track_count != header.track_count ||
            old_header.scan_signature != header.scan_signature ||
            old_header.root_file_count != header.root_file_count ||
            old_header.root_latest_mtime != header.root_latest_mtime ||
            old_header.root_total_size != header.root_total_size ||
            old_header.root_signature != header.root_signature) {
            ESP_LOGI(TAG, "[scan] file change detected old=%lu/%lu new=%lu/%lu",
                     (unsigned long)old_header.track_count,
                     (unsigned long)old_header.scan_signature,
                     (unsigned long)header.track_count,
                     (unsigned long)header.scan_signature);
        } else {
            cache_changed = false;
            ESP_LOGI(TAG, "[scan] file list unchanged count=%lu signature=%lu",
                     (unsigned long)header.track_count,
                     (unsigned long)header.scan_signature);
        }
    } else {
        ESP_LOGI(TAG, "[scan] first cache build count=%lu signature=%lu",
                 (unsigned long)header.track_count,
                 (unsigned long)header.scan_signature);
    }

    ESP_LOGI(TAG,
             "[scan] grouped cache mp3=%lu wav=%lu other=%lu total=%lu",
             (unsigned long)ctx.mp3_count,
             (unsigned long)ctx.wav_count,
             (unsigned long)ctx.other_count,
             (unsigned long)header.track_count);

    if (!cache_changed) {
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        if (activate_now) {
            s_sdcard_handler->track_total = old_header.track_count;
            s_sdcard_handler->track_scan_signature = old_header.scan_signature;
            ESP_LOGI(TAG, "[scan] keep existing cache without rewrite");
            return _cache_set_current_index(0U);
        }

        ESP_LOGI(TAG, "[scan] keep active cache, background stage unchanged");
        return ESP_OK;
    }

    if (!activate_now) {
        remove(SDCARD_CACHE_M3U_STAGE_PATH);
        remove(SDCARD_CACHE_IDX_STAGE_PATH);
        if (rename(SDCARD_CACHE_M3U_TMP_PATH, SDCARD_CACHE_M3U_STAGE_PATH) != 0 ||
            rename(SDCARD_CACHE_IDX_TMP_PATH, SDCARD_CACHE_IDX_STAGE_PATH) != 0) {
            remove(SDCARD_CACHE_M3U_TMP_PATH);
            remove(SDCARD_CACHE_IDX_TMP_PATH);
            ESP_LOGE(TAG, "[scan] commit stage cache failed");
            return ESP_FAIL;
        }

        s_sdcard_handler->cache_swap_pending = true;
        ESP_LOGI(TAG, "[scan] stage cache ready tracks=%lu pending_swap=1",
                 (unsigned long)header.track_count);
        return ESP_OK;
    }

    remove(SDCARD_CACHE_M3U_PATH);
    remove(SDCARD_CACHE_IDX_PATH);
    if (rename(SDCARD_CACHE_M3U_TMP_PATH, SDCARD_CACHE_M3U_PATH) != 0 ||
        rename(SDCARD_CACHE_IDX_TMP_PATH, SDCARD_CACHE_IDX_PATH) != 0) {
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        ESP_LOGE(TAG, "[scan] commit cache failed");
        return ESP_FAIL;
    }

    s_sdcard_handler->track_total = header.track_count;
    s_sdcard_handler->track_scan_signature = header.scan_signature;
    ESP_LOGI(TAG, "[scan] cache ready tracks=%lu limit=%u",
             (unsigned long)s_sdcard_handler->track_total,
             (unsigned int)SDCARD_SCAN_MAX_TRACKS);
    return _cache_set_current_index(0U);
}

/*
 * 增量追加：将单首歌写入 active M3U/IDX，并在扫到第 1 首时通知播放线程开播。
 * 调用者必须保证 ctx->active_m3u_fp 和 ctx->active_idx_fp 已打开。
 */
static esp_err_t _incremental_append_to_active_cache(sdcard_scan_context_t *ctx, const char *url)
{
    uint32_t line_offset = 0;
    sdcard_cache_idx_header_t header;

    if (ctx == NULL || url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->active_m3u_fp == NULL || ctx->active_idx_fp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 1) 追加 URL 到 M3U */
    line_offset = (uint32_t)ftell(ctx->active_m3u_fp);
    if (fprintf(ctx->active_m3u_fp, "%s\n", url) < 0) {
        ESP_LOGE(TAG, "[incremental] write m3u failed");
        return ESP_FAIL;
    }

    /* 2) 追加 offset 到 IDX（header 之后） */
    if (fseek(ctx->active_idx_fp, (long)(sizeof(sdcard_cache_idx_header_t) + ctx->active_track_count * sizeof(uint32_t)), SEEK_SET) != 0 ||
        fwrite(&line_offset, 1, sizeof(line_offset), ctx->active_idx_fp) != sizeof(line_offset)) {
        ESP_LOGE(TAG, "[incremental] write idx offset failed");
        return ESP_FAIL;
    }

    ctx->active_track_count++;

    /* 3) 更新 IDX header 中的 track_count */
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, SDCARD_CACHE_IDX_MAGIC, sizeof(header.magic) - 1U);
    header.version = SDCARD_CACHE_IDX_VERSION;
    header.track_count = ctx->active_track_count;
    if (fseek(ctx->active_idx_fp, 0L, SEEK_SET) != 0 ||
        fwrite(&header, 1, sizeof(header), ctx->active_idx_fp) != sizeof(header)) {
        ESP_LOGE(TAG, "[incremental] update idx header failed");
        return ESP_FAIL;
    }

    /* 4) fflush 保证播放线程可读 */
    fflush(ctx->active_m3u_fp);
    fflush(ctx->active_idx_fp);

    /* 5) 更新内存中 track_total */
    if (s_sdcard_handler != NULL) {
        s_sdcard_handler->track_total = ctx->active_track_count;
    }

    ESP_LOGD(TAG, "[incremental] appended #%lu: %s",
             (unsigned long)ctx->active_track_count, url);

    /* 6) 第 1 首通知开播 */
    if (!ctx->first_track_notified && s_sdcard_handler != NULL) {
        ctx->first_track_notified = true;
        s_sdcard_handler->track_index = 0U;

        /*
         * 设置 current_playlist_url 等字段，_on_play_pause → _build_current_track_plan 需要。
         * _cache_set_current_index 内部会读取已 fflush 的 active M3U/IDX 文件。
         */
        if (_cache_set_current_index(0U) == ESP_OK) {
            track_play_plan_t plan;
            if (_select_playable_track(&plan) == ESP_OK) {
                _remember_current_track(&plan);
                audio_element_set_uri(s_sdcard_handler->element_fatfs, plan.playback_url);
                app_player_sdcard_refresh_track_display();
            }
        }

        ESP_LOGI(TAG, "[incremental] first track ready, triggering playback");
        send_cmd(SDCARD_CMD_PLAY_PAUSE);
    }

    return ESP_OK;
}

/*
 * 增量扫描完成后：用分桶临时文件重写 active M3U/IDX，保证排序（MP3→WAV→其他）。
 * 调用前 ctx->active_m3u_fp / active_idx_fp 必须已关闭。
 */
static esp_err_t _incremental_finalize_cache(sdcard_scan_context_t *ctx)
{
    sdcard_cache_idx_header_t header;
    uint32_t merged_count = 0;
    esp_err_t ret = ESP_OK;
    char preferred_url[SDCARD_ACTIVE_URL_MAX_LEN];
    uint32_t target_index = 0U;

    if (ctx == NULL || s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* 记住当前正在播放的歌曲 URL，以便重排后恢复索引 */
    preferred_url[0] = '\0';
    mutex_lock(s_sdcard_handler->lock);
    if (s_sdcard_handler->current_playlist_url[0] != '\0') {
        snprintf(preferred_url, sizeof(preferred_url), "%s", s_sdcard_handler->current_playlist_url);
    }
    mutex_unlock(s_sdcard_handler->lock);

    /* 写排序结果到 TMP 文件 */
    remove(SDCARD_CACHE_M3U_TMP_PATH);
    remove(SDCARD_CACHE_IDX_TMP_PATH);

    ctx->m3u_fp = fopen(SDCARD_CACHE_M3U_TMP_PATH, "wb");
    ctx->idx_fp = fopen(SDCARD_CACHE_IDX_TMP_PATH, "wb+");
    if (ctx->m3u_fp == NULL || ctx->idx_fp == NULL) {
        if (ctx->m3u_fp != NULL) fclose(ctx->m3u_fp);
        if (ctx->idx_fp != NULL) fclose(ctx->idx_fp);
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        ESP_LOGE(TAG, "[incremental_finalize] create tmp files failed");
        return ESP_FAIL;
    }

    /* 写占位 header */
    memset(&header, 0, sizeof(header));
    fwrite(&header, 1, sizeof(header), ctx->idx_fp);

    /* merge 阶段重新累计 signature，保证与排序后的最终顺序一致 */
    ctx->signature = 2166136261UL;

    /* 如果当前有播放歌曲，将其强制置于播放列表的首位 (index 0)，以保证 UI 始终显示 001，避免突变 */
    if (preferred_url[0] != '\0') {
        uint32_t line_offset = (uint32_t)ftell(ctx->m3u_fp);
        size_t preferred_len = strlen(preferred_url);
        if (fprintf(ctx->m3u_fp, "%s\n", preferred_url) < 0 ||
            fwrite(&line_offset, 1, sizeof(line_offset), ctx->idx_fp) != sizeof(line_offset)) {
            ret = ESP_FAIL;
        } else {
            ctx->signature = _fnv1a_update(ctx->signature, preferred_url, preferred_len);
            merged_count++;
        }
    }

    /* 按 MP3 → WAV → Other 顺序 merge，且跳过已被置顶的 preferred_url */
    const char *skip_url = (preferred_url[0] != '\0') ? preferred_url : NULL;
    if (_merge_scan_bucket_to_cache(ctx->mp3_fp, ctx, &merged_count, skip_url) != ESP_OK ||
        _merge_scan_bucket_to_cache(ctx->wav_fp, ctx, &merged_count, skip_url) != ESP_OK ||
        _merge_scan_bucket_to_cache(ctx->other_fp, ctx, &merged_count, skip_url) != ESP_OK) {
        ret = ESP_FAIL;
    }
    fflush(ctx->m3u_fp);
    fflush(ctx->idx_fp);

    if (ret == ESP_OK && merged_count == 0U) {
        ret = ESP_FAIL;
    }

    if (ret == ESP_OK) {
        memset(&header, 0, sizeof(header));
        memcpy(header.magic, SDCARD_CACHE_IDX_MAGIC, sizeof(header.magic) - 1U);
        header.version = SDCARD_CACHE_IDX_VERSION;
        header.track_count = merged_count;
        header.scan_signature = ctx->signature;
        header.root_file_count = ctx->root_fp.file_count;
        header.root_latest_mtime = ctx->root_fp.latest_mtime;
        header.root_total_size = ctx->root_fp.total_size;
        header.root_signature = ctx->root_fp.signature;
        if (fseek(ctx->idx_fp, 0L, SEEK_SET) != 0 ||
            fwrite(&header, 1, sizeof(header), ctx->idx_fp) != sizeof(header)) {
            ret = ESP_FAIL;
        }
    }

    fclose(ctx->m3u_fp);
    fclose(ctx->idx_fp);
    ctx->m3u_fp = NULL;
    ctx->idx_fp = NULL;

    if (ret != ESP_OK) {
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        return ret;
    }

    /* 用排序后的 TMP 替换 active 文件 */
    remove(SDCARD_CACHE_M3U_PATH);
    remove(SDCARD_CACHE_IDX_PATH);
    if (rename(SDCARD_CACHE_M3U_TMP_PATH, SDCARD_CACHE_M3U_PATH) != 0 ||
        rename(SDCARD_CACHE_IDX_TMP_PATH, SDCARD_CACHE_IDX_PATH) != 0) {
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        ESP_LOGE(TAG, "[incremental_finalize] commit sorted cache failed");
        return ESP_FAIL;
    }

    /* 更新内存状态 */
    s_sdcard_handler->track_total = merged_count;
    s_sdcard_handler->track_scan_signature = header.scan_signature;

    /* 尝试恢复当前播放歌曲在新排序中的索引 */
    if (preferred_url[0] != '\0' &&
        _find_track_index_in_cache_m3u(SDCARD_CACHE_M3U_PATH, preferred_url, &target_index) == ESP_OK) {
        ESP_LOGD(TAG, "[incremental_finalize] restore track index=%lu url=%s",
                 (unsigned long)target_index, preferred_url);
    } else {
        target_index = 0U;
    }
    _cache_set_current_index(target_index);
    /* 排序完成后立即更新 UI，避免等到用户按切歌键时曲目号发生突变 */
    app_player_sdcard_refresh_track_display();

    ESP_LOGI(TAG,
             "[incremental_finalize] sorted cache mp3=%lu wav=%lu other=%lu total=%lu",
             (unsigned long)ctx->mp3_count,
             (unsigned long)ctx->wav_count,
             (unsigned long)ctx->other_count,
             (unsigned long)merged_count);

    return ESP_OK;
}

static bool _bucket_append_url(track_url_bucket_t *bucket, const char *url)
{
    char *copy = NULL;
    char **new_items = NULL;
    size_t new_capacity = 0;

    if (bucket == NULL || url == NULL) {
        return false;
    }

    if (bucket->count >= bucket->capacity) {
        new_capacity = (bucket->capacity == 0U) ? 8U : (bucket->capacity * 2U);
        new_items = (char **)realloc(bucket->items, new_capacity * sizeof(char *));
        if (new_items == NULL) {
            return false;
        }
        bucket->items = new_items;
        bucket->capacity = new_capacity;
    }

    copy = (char *)malloc(strlen(url) + 1U);
    if (copy == NULL) {
        return false;
    }
    memcpy(copy, url, strlen(url) + 1U);
    bucket->items[bucket->count++] = copy;
    return true;
}

static void _bucket_release(track_url_bucket_t *bucket)
{
    size_t index = 0;

    if (bucket == NULL) {
        return;
    }

    for (index = 0; index < bucket->count; index++) {
        if (bucket->items[index] != NULL) {
            free(bucket->items[index]);
            bucket->items[index] = NULL;
        }
    }
    free(bucket->items);
    bucket->items = NULL;
    bucket->count = 0U;
    bucket->capacity = 0U;
}

static esp_err_t _playlist_append_bucket(playlist_handle_t playlist, track_url_bucket_t *bucket)
{
    size_t index = 0;

    if (playlist == NULL || bucket == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    for (index = 0; index < bucket->count; index++) {
        if (bucket->items[index] == NULL) {
            continue;
        }
        if (playlist_save(playlist, bucket->items[index]) != ESP_OK) {
            ESP_LOGE(TAG, "[playlist_reorder] save failed: %s", bucket->items[index]);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t _create_playlist_handle(playlist_handle_t *out_playlist)
{
    playlist_operator_handle_t dram_list_handle = NULL;
    playlist_handle_t playlist = NULL;

    if (out_playlist == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    playlist = playlist_create();
    if (playlist == NULL) {
        return ESP_FAIL;
    }

    dram_list_create(&dram_list_handle);
    if (dram_list_handle == NULL) {
        playlist_destroy(playlist);
        return ESP_FAIL;
    }

    if (playlist_add(playlist, dram_list_handle, 1) != ESP_OK) {
        playlist_destroy(playlist);
        return ESP_FAIL;
    }

    *out_playlist = playlist;
    return ESP_OK;
}

static esp_err_t _rebuild_playlist_by_format(playlist_handle_t *playlist_handle)
{
    track_url_bucket_t mp3_bucket = {0};
    track_url_bucket_t wav_bucket = {0};
    track_url_bucket_t other_bucket = {0};
    playlist_handle_t old_playlist = NULL;
    playlist_handle_t new_playlist = NULL;
    char *url = NULL;
    int total = 0;
    int index = 0;
    esp_err_t ret = ESP_OK;

    if (playlist_handle == NULL || *playlist_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    old_playlist = *playlist_handle;
    total = playlist_get_current_list_url_num(old_playlist);
    if (total <= 1) {
        return ESP_OK;
    }

    for (index = 0; index < total; index++) {
        track_url_bucket_t *target_bucket = &other_bucket;

        if (playlist_get_current_list_url(old_playlist, &url) != ESP_OK || url == NULL) {
            ret = ESP_FAIL;
            break;
        }

        switch (_classify_track_format(url))
        {
        case TRACK_FORMAT_MP3:
            target_bucket = &mp3_bucket;
            break;
        case TRACK_FORMAT_WAV:
            target_bucket = &wav_bucket;
            break;
        case TRACK_FORMAT_OTHER:
        default:
            target_bucket = &other_bucket;
            break;
        }

        if (!_bucket_append_url(target_bucket, url)) {
            ESP_LOGE(TAG, "[playlist_reorder] append failed: %s", url);
            ret = ESP_ERR_NO_MEM;
            break;
        }

        if ((index + 1) < total) {
            playlist_next(old_playlist, 1, &url);
        }
    }

    if (ret != ESP_OK) {
        goto exit;
    }

    if (_create_playlist_handle(&new_playlist) != ESP_OK) {
        ret = ESP_FAIL;
        goto exit;
    }

    ret = _playlist_append_bucket(new_playlist, &mp3_bucket);
    if (ret == ESP_OK) {
        ret = _playlist_append_bucket(new_playlist, &wav_bucket);
    }
    if (ret == ESP_OK) {
        ret = _playlist_append_bucket(new_playlist, &other_bucket);
    }
    if (ret != ESP_OK) {
        goto exit;
    }

    playlist_destroy(old_playlist);
    *playlist_handle = new_playlist;
    new_playlist = NULL;

    ESP_LOGI(TAG,
             "[playlist_reorder] mp3=%u wav=%u other=%u total=%u",
             (unsigned int)mp3_bucket.count,
             (unsigned int)wav_bucket.count,
             (unsigned int)other_bucket.count,
             (unsigned int)(mp3_bucket.count + wav_bucket.count + other_bucket.count));

exit:
    if (new_playlist != NULL) {
        playlist_destroy(new_playlist);
    }
    _bucket_release(&mp3_bucket);
    _bucket_release(&wav_bucket);
    _bucket_release(&other_bucket);
    return ret;
}

static const char *_uri_to_local_path(const char *url)
{
    static const char file_prefix[] = "file://";
    static char local_path[SDCARD_ACTIVE_URL_MAX_LEN];
    const char *path = NULL;
    size_t path_len = 0;

    if (url == NULL) {
        return NULL;
    }
    if (strncmp(url, file_prefix, sizeof(file_prefix) - 1) == 0) {
        path = url + (sizeof(file_prefix) - 1);
        if (strncmp(path, "/sdcard/", 8) == 0) {
            return path;
        }
        if (strncmp(path, "sdcard/", 7) == 0) {
            snprintf(local_path, sizeof(local_path), "/%s", path);
            return local_path;
        }
        if (path[0] != '\0') {
            path_len = strlen(path);
            if (path_len + 2U <= sizeof(local_path)) {
                snprintf(local_path, sizeof(local_path), "/%s", path);
                return local_path;
            }
        }
    }
    return url;
}

typedef struct
{
    uint32_t frame_size;
} mp3_frame_desc_t;

static bool _parse_mp3_frame_header(const uint8_t *header, mp3_frame_desc_t *desc)
{
    static const uint16_t bitrate_v1_l1[16] = {
        0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0
    };
    static const uint16_t bitrate_v1_l2[16] = {
        0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0
    };
    static const uint16_t bitrate_v1_l3[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0
    };
    static const uint16_t bitrate_v2_l1[16] = {
        0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0
    };
    static const uint16_t bitrate_v2_l23[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0
    };
    static const uint16_t sample_rate_tbl[4][4] = {
        {11025, 12000, 8000, 0},
        {0, 0, 0, 0},
        {22050, 24000, 16000, 0},
        {44100, 48000, 32000, 0}
    };
    uint8_t version_id;
    uint8_t layer_id;
    uint8_t bitrate_idx;
    uint8_t sample_rate_idx;
    uint8_t padding_bit;
    uint32_t sample_rate;
    uint32_t bitrate_kbps;
    uint32_t frame_size;

    if (header == NULL || desc == NULL) {
        return false;
    }
    if (header[0] != 0xFFU || (header[1] & 0xE0U) != 0xE0U) {
        return false;
    }

    version_id = (uint8_t)((header[1] >> 3) & 0x03U);
    layer_id = (uint8_t)((header[1] >> 1) & 0x03U);
    bitrate_idx = (uint8_t)((header[2] >> 4) & 0x0FU);
    sample_rate_idx = (uint8_t)((header[2] >> 2) & 0x03U);
    padding_bit = (uint8_t)((header[2] >> 1) & 0x01U);

    if (version_id == 0x01U || layer_id == 0x00U) {
        return false;
    }
    if (bitrate_idx == 0x00U || bitrate_idx == 0x0FU || sample_rate_idx == 0x03U) {
        return false;
    }

    sample_rate = sample_rate_tbl[version_id][sample_rate_idx];
    if (sample_rate == 0U) {
        return false;
    }

    if (version_id == 0x03U) {
        if (layer_id == 0x03U) {
            bitrate_kbps = bitrate_v1_l1[bitrate_idx];
        } else if (layer_id == 0x02U) {
            bitrate_kbps = bitrate_v1_l2[bitrate_idx];
        } else {
            bitrate_kbps = bitrate_v1_l3[bitrate_idx];
        }
    } else {
        if (layer_id == 0x03U) {
            bitrate_kbps = bitrate_v2_l1[bitrate_idx];
        } else {
            bitrate_kbps = bitrate_v2_l23[bitrate_idx];
        }
    }

    if (bitrate_kbps == 0U) {
        return false;
    }

    if (layer_id == 0x03U) {
        frame_size = (((12U * bitrate_kbps * 1000U) / sample_rate) + padding_bit) * 4U;
    } else if (layer_id == 0x01U && version_id != 0x03U) {
        frame_size = ((72U * bitrate_kbps * 1000U) / sample_rate) + padding_bit;
    } else {
        frame_size = ((144U * bitrate_kbps * 1000U) / sample_rate) + padding_bit;
    }

    if (frame_size < 24U) {
        return false;
    }

    desc->frame_size = frame_size;
    return true;
}

static bool _verify_mp3_frame_at(FILE *fp, long file_size, uint32_t frame_offset, mp3_frame_desc_t *desc)
{
    uint8_t next_header[4];
    mp3_frame_desc_t next_desc;
    uint32_t next_offset;

    if (fp == NULL || desc == NULL || desc->frame_size == 0U) {
        return false;
    }

    next_offset = frame_offset + desc->frame_size;
    if (next_offset >= (uint32_t)file_size) {
        return true;
    }
    if ((long)(next_offset + sizeof(next_header)) > file_size) {
        return true;
    }

    if (fseek(fp, (long)next_offset, SEEK_SET) != 0) {
        return false;
    }
    if (fread(next_header, 1, sizeof(next_header), fp) != sizeof(next_header)) {
        return false;
    }

    return _parse_mp3_frame_header(next_header, &next_desc);
}

/*
 * MP3 快速预检：
 * 1. 仅处理头部区域，不做完整解码。
 * 2. 先识别并跳过 ID3v2 大标签。
 * 3. 在有限窗口内搜索首个合法帧同步字，并用下一帧复核，降低误判成 PCM 的概率。
 * 4. 头部脏数据过大或始终找不到合法帧时，直接判为不可播放。
 */
static esp_err_t _scan_mp3_file(const char *url, mp3_scan_info_t *info)
{
    uint8_t header[10];
    FILE *fp = NULL;
    const char *local_path = NULL;
    long file_size = 0;
    uint32_t search_start = 0;
    uint32_t search_end = 0;
    uint32_t chunk_base = 0;

    if (url == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    if (s_sdcard_handler != NULL) {
        s_sdcard_handler->stats.mp3_check_count++;
    }

    local_path = _uri_to_local_path(url);
    fp = fopen(local_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "[mp3_fast_check] open failed: %s, errno: %d", local_path, errno);
        _record_mp3_check_fail("open_failed", url);
        return ESP_FAIL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        _record_mp3_check_fail("seek_end", url);
        return ESP_FAIL;
    }

    file_size = ftell(fp);
    if (file_size < 4L || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        _record_mp3_check_fail("file_short", url);
        return ESP_FAIL;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        if (fseek(fp, 0, SEEK_SET) != 0 || fread(header, 1, 4, fp) != 4U) {
            fclose(fp);
            _record_mp3_check_fail("header_read", url);
            return ESP_FAIL;
        }
        memset(&header[4], 0, sizeof(header) - 4U);
    }

    if (memcmp(header, "ID3", 3) == 0) {
        uint32_t id3_body_size = _read_syncsafe_u32(&header[6]);
        uint32_t total_skip = 0;

        if (id3_body_size == UINT32_MAX) {
            fclose(fp);
            _record_mp3_check_fail("id3_size", url);
            return ESP_FAIL;
        }

        total_skip = 10U + id3_body_size;
        if ((header[5] & 0x10U) != 0U) {
            total_skip += 10U;
        }
        if (total_skip >= (uint32_t)file_size) {
            fclose(fp);
            _record_mp3_check_fail("id3_overflow", url);
            return ESP_FAIL;
        }
        if (total_skip > SDCARD_MP3_MAX_DIRTY_HEAD_BYTES) {
            fclose(fp);
            _record_mp3_check_fail("id3_too_large", url);
            return ESP_FAIL;
        }

        info->has_id3v2 = true;
        info->id3v2_size = total_skip;
        search_start = total_skip;
    }

    if (search_start > SDCARD_MP3_MAX_DIRTY_HEAD_BYTES) {
        fclose(fp);
        _record_mp3_check_fail("dirty_head_too_large", url);
        return ESP_FAIL;
    }

    search_end = search_start + SDCARD_MP3_HEAD_SCAN_LIMIT_BYTES;
    if (search_end > (uint32_t)file_size) {
        search_end = (uint32_t)file_size;
    }

    chunk_base = search_start;
    while (chunk_base < search_end) {
        size_t read_len = (size_t)(search_end - chunk_base);
        size_t bytes_read = 0;
        size_t index = 0;

        if (read_len > sizeof(s_mp3_scan_buf)) {
            read_len = sizeof(s_mp3_scan_buf);
        }
        if (fseek(fp, (long)chunk_base, SEEK_SET) != 0) {
            fclose(fp);
            _record_mp3_check_fail("scan_seek", url);
            return ESP_FAIL;
        }
        bytes_read = fread(s_mp3_scan_buf, 1, read_len, fp);
        if (bytes_read < 4U) {
            break;
        }

        for (index = 0; (index + 4U) <= bytes_read; index++) {
            mp3_frame_desc_t desc;
            uint32_t candidate = chunk_base + (uint32_t)index;

            if (!_parse_mp3_frame_header(&s_mp3_scan_buf[index], &desc)) {
                continue;
            }
            if (candidate > SDCARD_MP3_MAX_DIRTY_HEAD_BYTES) {
                fclose(fp);
                _record_mp3_check_fail("audio_offset_too_large", url);
                return ESP_FAIL;
            }
            if (_verify_mp3_frame_at(fp, file_size, candidate, &desc)) {
                info->audio_offset = candidate;
                fclose(fp);
                ESP_LOGD(TAG,
                         "[mp3_fast_check] frame found offset=%lu id3=%lu url=%s",
                         (unsigned long)info->audio_offset,
                         (unsigned long)info->id3v2_size,
                         url);
                return ESP_OK;
            }
        }

        if (bytes_read <= 4U) {
            break;
        }
        chunk_base += (uint32_t)(bytes_read - 3U);
    }

    fclose(fp);
    _record_mp3_check_fail("frame_sync_not_found", url);
    return ESP_FAIL;
}

/*
 * 顺序扫描 WAV chunk，拿到 fmt/data 的真实位置。
 * 这样可以处理 data chunk 前存在大块 LIST/JUNK/封面数据的情况。
 */
static esp_err_t _scan_wav_file(const char *url, wav_scan_info_t *info)
{
    uint8_t header[12];
    uint8_t chunk_header[8];
    uint8_t fmt_data[16];
    FILE *fp = NULL;
    const char *local_path = NULL;
    uint32_t offset = 12;
    bool fmt_found = false;
    bool data_found = false;
    long file_size = 0;

    if (url == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(info, 0, sizeof(*info));
    s_sdcard_handler->stats.wav_check_count++;
    local_path = _uri_to_local_path(url);

    fp = fopen(local_path, "rb");
    if (fp == NULL) {
        ESP_LOGE(TAG, "[wav_fast_check] open failed: %s, errno: %d", local_path, errno);
        _record_wav_check_fail("open_failed", url);
        return ESP_FAIL;
    }

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        _record_wav_check_fail("seek_end", url);
        return ESP_FAIL;
    }

    file_size = ftell(fp);
    if (file_size < 44 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        _record_wav_check_fail("header_short", url);
        return ESP_FAIL;
    }

    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        _record_wav_check_fail("header_read", url);
        return ESP_FAIL;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(&header[8], "WAVE", 4) != 0) {
        fclose(fp);
        _record_wav_check_fail("riff_wave", url);
        return ESP_FAIL;
    }

    while ((offset + 8U) <= (uint32_t)file_size) {
        uint32_t chunk_size = 0;
        uint32_t chunk_padding = 0;

        if (fread(chunk_header, 1, sizeof(chunk_header), fp) != sizeof(chunk_header)) {
            break;
        }

        chunk_size = _read_le32(&chunk_header[4]);
        chunk_padding = chunk_size & 0x1U;

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint16_t audio_format = 0;

            if (chunk_size < sizeof(fmt_data) || fread(fmt_data, 1, sizeof(fmt_data), fp) != sizeof(fmt_data)) {
                fclose(fp);
                _record_wav_check_fail("fmt_chunk", url);
                return ESP_FAIL;
            }

            audio_format = _read_le16(&fmt_data[0]);
            info->channels = _read_le16(&fmt_data[2]);
            info->sample_rate = _read_le32(&fmt_data[4]);
            info->bits_per_sample = _read_le16(&fmt_data[14]);

            if (audio_format != 1U) {
                fclose(fp);
                _record_wav_check_fail("audio_format", url);
                return ESP_FAIL;
            }
            if (!(info->channels == 1U || info->channels == 2U)) {
                fclose(fp);
                _record_wav_check_fail("channels", url);
                return ESP_FAIL;
            }
            if (!(info->bits_per_sample == 8U || info->bits_per_sample == 16U ||
                  info->bits_per_sample == 24U || info->bits_per_sample == 32U)) {
                fclose(fp);
                _record_wav_check_fail("bits", url);
                return ESP_FAIL;
            }
            if (info->sample_rate == 0U) {
                fclose(fp);
                _record_wav_check_fail("sample_rate", url);
                return ESP_FAIL;
            }

            if (chunk_size > sizeof(fmt_data) &&
                fseek(fp, (long)(chunk_size - sizeof(fmt_data)), SEEK_CUR) != 0) {
                fclose(fp);
                _record_wav_check_fail("fmt_seek", url);
                return ESP_FAIL;
            }
            if (chunk_padding && fseek(fp, 1L, SEEK_CUR) != 0) {
                fclose(fp);
                _record_wav_check_fail("fmt_pad", url);
                return ESP_FAIL;
            }
            fmt_found = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            if (!fmt_found) {
                fclose(fp);
                _record_wav_check_fail("data_before_fmt", url);
                return ESP_FAIL;
            }
            info->data_offset = offset + 8U;
            info->data_size = chunk_size;
            data_found = true;
            break;
        } else {
            if ((long)chunk_size > 0 && fseek(fp, (long)chunk_size, SEEK_CUR) != 0) {
                fclose(fp);
                _record_wav_check_fail("chunk_seek", url);
                return ESP_FAIL;
            }
            if (chunk_padding && fseek(fp, 1L, SEEK_CUR) != 0) {
                fclose(fp);
                _record_wav_check_fail("chunk_pad", url);
                return ESP_FAIL;
            }
        }

        if ((UINT32_MAX - offset) < (8U + chunk_size + chunk_padding)) {
            fclose(fp);
            _record_wav_check_fail("offset_overflow", url);
            return ESP_FAIL;
        }
        offset += 8U + chunk_size + chunk_padding;
    }

    if (!fmt_found || !data_found) {
        fclose(fp);
        _record_wav_check_fail("missing_fmt_data", url);
        return ESP_FAIL;
    }

    if ((long)(info->data_offset + info->data_size) > file_size) {
        info->data_size = (uint32_t)((file_size > (long)info->data_offset) ? (file_size - (long)info->data_offset) : 0);
    }
    info->requires_shadow = (info->data_offset > SDCARD_WAV_BYPASS_THRESHOLD_BYTES);
    fclose(fp);
    return ESP_OK;
}

/*
 * 生成一个“干净影子 WAV”，保留标准 44 字节 PCM 头，只复制真正的 data chunk。
 * 这样既能绕过 data 前的大块脏数据，又不需要改动现有 ESP-ADF 解码链。
 */
static esp_err_t _build_wav_shadow_file(const char *source_url, const wav_scan_info_t *info)
{
    uint8_t io_buf[1024];
    uint8_t header[44];
    FILE *src = NULL;
    FILE *dst = NULL;
    const char *local_path = NULL;
    uint32_t remain = 0;
    uint16_t block_align = 0;
    uint32_t byte_rate = 0;

    if (source_url == NULL || info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (info->channels == 0U || info->bits_per_sample == 0U || info->data_size == 0U) {
        return ESP_FAIL;
    }

    block_align = (uint16_t)(info->channels * (info->bits_per_sample / 8U));
    if (block_align == 0U) {
        return ESP_FAIL;
    }
    byte_rate = info->sample_rate * block_align;
    local_path = _uri_to_local_path(source_url);

    remove(SDCARD_WAV_SHADOW_PATH);
    src = fopen(local_path, "rb");
    if (src == NULL) {
        _record_wav_check_fail("shadow_src", source_url);
        return ESP_FAIL;
    }

    dst = fopen(SDCARD_WAV_SHADOW_PATH, "wb");
    if (dst == NULL) {
        fclose(src);
        _record_wav_check_fail("shadow_dst", source_url);
        return ESP_FAIL;
    }

    memcpy(&header[0], "RIFF", 4);
    _write_le32(&header[4], 36U + info->data_size);
    memcpy(&header[8], "WAVE", 4);
    memcpy(&header[12], "fmt ", 4);
    _write_le32(&header[16], 16U);
    _write_le16(&header[20], 1U);
    _write_le16(&header[22], info->channels);
    _write_le32(&header[24], info->sample_rate);
    _write_le32(&header[28], byte_rate);
    _write_le16(&header[32], block_align);
    _write_le16(&header[34], info->bits_per_sample);
    memcpy(&header[36], "data", 4);
    _write_le32(&header[40], info->data_size);

    if (fwrite(header, 1, sizeof(header), dst) != sizeof(header) ||
        fseek(src, (long)info->data_offset, SEEK_SET) != 0) {
        fclose(src);
        fclose(dst);
        remove(SDCARD_WAV_SHADOW_PATH);
        _record_wav_check_fail("shadow_head", source_url);
        return ESP_FAIL;
    }

    remain = info->data_size;
    while (remain > 0U) {
        size_t once = (remain > sizeof(io_buf)) ? sizeof(io_buf) : remain;
        size_t read_size = fread(io_buf, 1, once, src);
        if (read_size == 0U) {
            break;
        }
        if (fwrite(io_buf, 1, read_size, dst) != read_size) {
            fclose(src);
            fclose(dst);
            remove(SDCARD_WAV_SHADOW_PATH);
            _record_wav_check_fail("shadow_write", source_url);
            return ESP_FAIL;
        }
        remain -= (uint32_t)read_size;
    }

    fclose(src);
    fclose(dst);
    if (remain != 0U) {
        remove(SDCARD_WAV_SHADOW_PATH);
        _record_wav_check_fail("shadow_copy", source_url);
        return ESP_FAIL;
    }

    ESP_LOGW(TAG,
             "[wav_bypass] shadow ready src=%s offset=%lu bytes=%lu",
             source_url,
             (unsigned long)info->data_offset,
             (unsigned long)info->data_size);
    return ESP_OK;
}

static esp_err_t _prepare_wav_play_plan(const char *url, track_play_plan_t *plan)
{
    if (url == NULL || plan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (_scan_wav_file(url, &plan->wav_info) != ESP_OK) {
        ESP_LOGW(TAG, "[wav_bypass] scan failed, skip invalid wav: %s", url);
        return ESP_FAIL;
    }

    if (plan->wav_info.requires_shadow) {
        if (_build_wav_shadow_file(url, &plan->wav_info) != ESP_OK) {
            ESP_LOGW(TAG, "[wav_bypass] shadow build failed, skip wav: %s", url);
            return ESP_FAIL;
        }
        snprintf(plan->playback_url, sizeof(plan->playback_url), "%s", SDCARD_WAV_SHADOW_PATH);
        plan->use_shadow_file = true;
    } else {
        snprintf(plan->playback_url, sizeof(plan->playback_url), "%s", url);
        plan->use_shadow_file = false;
    }

    return ESP_OK;
}

static esp_err_t _prepare_mp3_play_plan(const char *url, track_play_plan_t *plan)
{
    if (url == NULL || plan == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (_scan_mp3_file(url, &plan->mp3_info) != ESP_OK) {
        ESP_LOGW(TAG, "[mp3_fast_check] skip invalid mp3: %s", url);
        return ESP_FAIL;
    }

    snprintf(plan->playback_url, sizeof(plan->playback_url), "%s", url);
    plan->start_offset = plan->mp3_info.audio_offset;
    return ESP_OK;
}

/*
 * 从当前 playlist 位置开始，向后寻找可播放曲目。
 * WAV 先做应用层解析；如 data 前存在大块脏数据，则自动落地为干净影子文件播放。
 */
static esp_err_t _select_playable_track(track_play_plan_t *plan)
{
    uint32_t total = 0;
    uint32_t attempt = 0;

    if (plan == NULL || s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(plan, 0, sizeof(*plan));
    total = s_sdcard_handler->track_total;
    if (total == 0U || _cache_refresh_neighbors() != ESP_OK) {
        return ESP_FAIL;
    }

    for (attempt = 0; attempt < total; attempt++) {
        const char *candidate = s_sdcard_handler->current_playlist_url;

        if (candidate == NULL || candidate[0] == '\0') {
            break;
        }

        plan->playlist_url = candidate;
        snprintf(plan->playback_url, sizeof(plan->playback_url), "%s", candidate);
        plan->format = _classify_track_format(candidate);
        plan->is_wav_track = _is_wav_url(candidate);
        plan->use_shadow_file = false;
        plan->start_offset = 0U;
        memset(&plan->mp3_info, 0, sizeof(plan->mp3_info));
        memset(&plan->wav_info, 0, sizeof(plan->wav_info));

        if (plan->format == TRACK_FORMAT_MP3) {
            if (_prepare_mp3_play_plan(candidate, plan) == ESP_OK) {
                break;
            }
            s_sdcard_handler->stats.mp3_skip_count++;
            ESP_LOGW(TAG, "[select_playable_track] skip invalid mp3: %s", candidate);
            _log_diag_stats("mp3_skip");
        } else if (!plan->is_wav_track || _prepare_wav_play_plan(candidate, plan) == ESP_OK) {
            break;
        } else {
            s_sdcard_handler->stats.wav_skip_count++;
            ESP_LOGW(TAG, "[select_playable_track] skip invalid wav: %s", candidate);
            _log_diag_stats("wav_skip");
        }

        memset(plan, 0, sizeof(*plan));
        if ((attempt + 1U) < total) {
            _cache_set_current_index((s_sdcard_handler->track_index + 1U) % total);
        }
    }

    if (plan->playlist_url == NULL) {
        ESP_LOGE(TAG, "[select_playable_track] no playable track found");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void _clear_current_track_context(void)
{
    if (s_sdcard_handler == NULL) {
        return;
    }
    _reset_low_water_guard_state(false, false);
    s_sdcard_handler->prev_playlist_url[0] = '\0';
    s_sdcard_handler->current_playlist_url[0] = '\0';
    s_sdcard_handler->current_playback_url[0] = '\0';
    s_sdcard_handler->current_track_start_offset = 0U;
    s_sdcard_handler->next_playlist_url[0] = '\0';
    s_sdcard_handler->track_total = 0U;
    s_sdcard_handler->track_index = 0U;
    s_sdcard_handler->current_track_format = TRACK_FORMAT_UNKNOWN;
    s_sdcard_handler->current_track_is_wav = false;
    s_sdcard_handler->current_track_play_tick = 0;
    s_sdcard_handler->soft_switch_mute_active = false;
    s_sdcard_handler->startup_guard_active = false;
    s_sdcard_handler->resume_guard_active = false;
    s_sdcard_handler->pause_transition_active = false;
    s_sdcard_handler->resume_guard_enter_tick = 0;
    s_sdcard_handler->startup_stable_hits = 0;
    s_sdcard_handler->auto_next_pending = false;
    s_sdcard_handler->auto_next_wait_tick = 0;
    s_sdcard_handler->auto_next_stable_hits = 0;
}

static void _reset_low_water_guard_state(bool keep_read_freeze, bool keep_silence_wait)
{
    if (s_sdcard_handler == NULL) {
        return;
    }

    if (!keep_read_freeze) {
        app_player_set_base_stream_read_freeze(false);
    }
    if (!keep_silence_wait) {
        app_player_set_base_stream_silence_wait(false);
    }

    app_player_set_base_stream_low_water_duck(false, 0);
    s_sdcard_handler->low_water_guard_active = false;
    s_sdcard_handler->low_water_enter_hits = 0;
    s_sdcard_handler->low_water_recover_hits = 0;
    s_sdcard_handler->low_water_duck_state = LOW_WATER_DUCK_STATE_IDLE;
    s_sdcard_handler->low_water_enter_tick = 0;
    s_sdcard_handler->low_water_saved_volume = 0;
    s_sdcard_handler->low_water_duck_volume = 0;
}

static void _remember_current_track(const track_play_plan_t *plan)
{
    if (s_sdcard_handler == NULL || plan == NULL || plan->playlist_url == NULL) {
        return;
    }

    snprintf(s_sdcard_handler->current_playlist_url,
             sizeof(s_sdcard_handler->current_playlist_url),
             "%s",
             plan->playlist_url);
    snprintf(s_sdcard_handler->current_playback_url,
             sizeof(s_sdcard_handler->current_playback_url),
             "%s",
             plan->playback_url);
    s_sdcard_handler->current_track_start_offset = plan->start_offset;
    s_sdcard_handler->current_track_format = plan->format;
    s_sdcard_handler->current_track_is_wav = plan->is_wav_track;
    s_sdcard_handler->startup_guard_active = (plan->is_wav_track || s_sdcard_handler->soft_switch_mute_active);
    s_sdcard_handler->startup_stable_hits = 0;
}

static sdcard_prio_policy_t _get_track_prio_policy(const track_play_plan_t *plan)
{
    if (plan != NULL && plan->is_wav_track) {
        return SDCARD_PRIO_POLICY_WAV_PRODUCER_FIRST;
    }

    return SDCARD_PRIO_POLICY_DEFAULT;
}

static float _get_track_preload_threshold(const track_play_plan_t *plan)
{
    if (plan != NULL && plan->is_wav_track) {
        return SDCARD_WAV_PRELOAD_THRESHOLD;
    }
    return SDCARD_MP3_PRELOAD_THRESHOLD;
}

/*
 * 统一计算安全恢复水位：
 * 1. 不超过 ringbuffer 的 3/4，避免目标过高导致长时间等待。
 * 2. 始终保证大于 min_level，形成明确的恢复迟滞区间。
 */
static int _calc_safe_buffer_level(int rb_size, int min_level, int safe_level)
{
    if (rb_size <= 0) {
        return safe_level;
    }

    if (min_level > (rb_size / 2)) {
        min_level = rb_size / 2;
    }
    if (safe_level > ((rb_size * 3) / 4)) {
        safe_level = (rb_size * 3) / 4;
    }
    if (safe_level <= min_level) {
        safe_level = min_level + (SDCARD_LOW_WATER_SILENCE_CHUNK / 2);
    }
    if (safe_level > rb_size) {
        safe_level = rb_size;
    }
    return safe_level;
}

static int _calc_mp3_startup_safe_level(int rb_size)
{
    int safe_level = 0;

    if (rb_size <= 0) {
        return SDCARD_MP3_STARTUP_SAFE_MIN_LEVEL;
    }

    safe_level = rb_size / 3;
    if (safe_level < SDCARD_MP3_STARTUP_SAFE_MIN_LEVEL) {
        safe_level = SDCARD_MP3_STARTUP_SAFE_MIN_LEVEL;
    }
    if (safe_level > (rb_size / 2)) {
        safe_level = rb_size / 2;
    }

    return safe_level;
}

/*
 * 统一计算运行期低水位门限：
 * 1. WAV 保持现有更强的保护力度。
 * 2. MP3 参考首播门限再适当提高，降低运行期偶发爆音概率。
 * 3. 同时输出进入/恢复命中次数，让不同格式有各自的保护灵敏度。
 */
static void _calc_track_low_water_levels(track_format_t format,
                                         int rb_size,
                                         int *min_level,
                                         int *safe_level,
                                         uint8_t *enter_hits_target,
                                         uint8_t *recover_hits_target)
{
    int local_min = SDCARD_LOW_WATER_MIN_LEVEL;
    int local_safe = SDCARD_LOW_WATER_SAFE_LEVEL;
    uint8_t local_enter_hits = SDCARD_LOW_WATER_ENTER_HITS;
    uint8_t local_recover_hits = SDCARD_LOW_WATER_RECOVER_HITS;

    if (format == TRACK_FORMAT_MP3) {
        int startup_safe = _calc_mp3_startup_safe_level(rb_size);

        /*
         * MP3 的运行期门限不是直接使用 WAV 常量，而是基于 MP3 首播安全水位再抬高。
         * 这样既能保留 MP3 对缓存波动的容忍度，又能避免运行中偶发掉水位时马上爆音。
         */
        local_min = startup_safe + SDCARD_MP3_LOW_WATER_MIN_EXTRA;
        local_safe = startup_safe + SDCARD_MP3_LOW_WATER_SAFE_EXTRA;
        local_enter_hits = SDCARD_MP3_LOW_WATER_ENTER_HITS;
        local_recover_hits = SDCARD_MP3_LOW_WATER_RECOVER_HITS;
    }

    /* 对目标 safe_level 做统一裁剪，确保它既比 min_level 大，又不会大到长期达不到。 */
    local_safe = _calc_safe_buffer_level(rb_size, local_min, local_safe);
    if (local_min > (rb_size / 2)) {
        local_min = rb_size / 2;
    }

    if (min_level != NULL) {
        *min_level = local_min;
    }
    if (safe_level != NULL) {
        *safe_level = local_safe;
    }
    if (enter_hits_target != NULL) {
        *enter_hits_target = local_enter_hits;
    }
    if (recover_hits_target != NULL) {
        *recover_hits_target = local_recover_hits;
    }
}

/*
 * 起播保护：
 * 1. WAV 首播时，防止冷缓存导致刚出声就掉水位。
 * 2. 手动软切歌时，保持静音直到新歌 PCM 缓冲连续稳定，再真正放音。
 * 3. 这里检查的是最终输出 RB，而不是 ADF 元素内部输入 RB，更贴近真实听感风险。
 */
static bool _wait_track_startup_buffer_ready(void)
{
    ringbuf_handle_t output_rb = NULL;
    int rb_size = 0;
    int filled = 0;
    int safe_level = 0;
    uint8_t stable_hits_target = 0;

    if (s_sdcard_handler == NULL) {
        return false;
    }
    if (!s_sdcard_handler->startup_guard_active) {
        return false;
    }

    output_rb = s_sdcard_handler->output_rb;
    if (output_rb == NULL) {
        return false;
    }

    rb_size = rb_get_size(output_rb);
    if (rb_size <= 0) {
        return false;
    }

    /* 以最终输出 RB 的已填充字节数作为“是否可以开始放音”的唯一依据。 */
    filled = rb_bytes_filled(output_rb);
    if (s_sdcard_handler->current_track_is_wav) {
        /* WAV 更容易在首播时被瞬时消耗空，因此直接沿用较高的低水位安全配置。 */
        safe_level = _calc_safe_buffer_level(rb_size,
                                             SDCARD_LOW_WATER_MIN_LEVEL,
                                             SDCARD_LOW_WATER_SAFE_LEVEL);
        stable_hits_target = SDCARD_STARTUP_STABLE_HITS;
    } else {
        /* MP3 解码后补仓相对更平滑，首播安全门限可以低一些，加快出声速度。 */
        safe_level = _calc_mp3_startup_safe_level(rb_size);
        stable_hits_target = 2U;
    }

    if (filled >= safe_level) {
        if (s_sdcard_handler->startup_stable_hits < 0xFFU) {
            s_sdcard_handler->startup_stable_hits++;
        }
    } else {
        s_sdcard_handler->startup_stable_hits = 0;
    }

    /*
     * 只有连续多次都达到安全水位，才真正解除冻结与静音等待。
     * 这样可以避免“刚摸到阈值就放音，下一拍又跌回去”的来回抖动。
     */
    if (s_sdcard_handler->startup_stable_hits >= stable_hits_target) {
        bool was_soft_switch = s_sdcard_handler->soft_switch_mute_active;
        _log_sd_read_cost("startup_ready",
                          s_sdcard_handler->loading_enter_tick,
                          filled,
                          rb_size,
                          safe_level);
        _log_output_rb_watermark("startup_ready", filled, rb_size, safe_level, safe_level);
#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_LOW_WATER
        _dump_pipeline_cache_status("startup_ready");
#endif
        s_sdcard_handler->startup_guard_active = false;
        s_sdcard_handler->startup_stable_hits = 0;
        s_sdcard_handler->soft_switch_mute_active = false;
        if (!s_sdcard_handler->pause_transition_active) {
            app_player_set_base_stream_read_freeze(false);
            app_player_set_base_stream_silence_wait(false);
        }
        ESP_LOGI(TAG, "[startup_guard] ready track wav=%d soft=%d filled=%d safe=%d",
                 s_sdcard_handler->current_track_is_wav ? 1 : 0,
                 was_soft_switch ? 1 : 0,
                 filled,
                 safe_level);
        return false;
    }

    /*
     * 预缓存阶段的核心动作：
     * 1. 冻结 mixer 对 base stream 的真实读取，避免刚产生的 PCM 被立即消耗。
     * 2. 打开 silence wait，让输出侧保持静音等待状态。
     * 3. 周期性复查，直到缓存稳定后再真正放音。
     */
    app_player_set_base_stream_read_freeze(true);
    app_player_set_base_stream_silence_wait(true);
    (void)_schedule_loading_state_check(SDCARD_STARTUP_GUARD_CHECK_MS, "startup_guard");
    ESP_LOGI(TAG, "[startup_guard] wait track wav=%d soft=%d filled=%d safe=%d hits=%u/%u rb=%d",
             s_sdcard_handler->current_track_is_wav ? 1 : 0,
             s_sdcard_handler->soft_switch_mute_active ? 1 : 0,
             filled,
             safe_level,
             (unsigned int)s_sdcard_handler->startup_stable_hits,
             (unsigned int)stable_hits_target,
             rb_size);
    return true;
}

/*
 * 自动切歌等待：
 * 1. rsp_filter 上报 FINISHED 后，先不要立刻切下一首。
 * 2. 保持 mixer 读取真实输出 RB，让已经解码的尾部 PCM 自然播放完。
 * 3. 缓存排空后进入原有切歌流程；若排空异常，则超时裁掉尾部 PCM，避免卡死。
 */
static bool _handle_auto_next_wait(void)
{
    ringbuf_handle_t output_rb = NULL;
    TickType_t now_tick = 0;
    TickType_t elapsed = 0;
    int filled = 0;
    int rb_size = 0;
    bool drain_ready = false;
    bool drain_timeout = false;

    if (s_sdcard_handler == NULL || !s_sdcard_handler->auto_next_pending) {
        return false;
    }

    output_rb = s_sdcard_handler->output_rb;
    if (output_rb == NULL) {
        output_rb = audio_element_get_input_ringbuf(s_sdcard_handler->element_raw);
    }

    now_tick = xTaskGetTickCount();
    if (s_sdcard_handler->auto_next_wait_tick != 0) {
        elapsed = now_tick - s_sdcard_handler->auto_next_wait_tick;
    }

    if (output_rb != NULL) {
        filled = rb_bytes_filled(output_rb);
        rb_size = rb_get_size(output_rb);
    } else {
        filled = -1;
        rb_size = 0;
    }

    if (filled >= 0 && filled <= SDCARD_AUTO_NEXT_DRAIN_THRESHOLD_BYTES) {
        if (s_sdcard_handler->auto_next_stable_hits < 0xFFU) {
            s_sdcard_handler->auto_next_stable_hits++;
        }
    } else {
        s_sdcard_handler->auto_next_stable_hits = 0;
    }

    drain_ready = (s_sdcard_handler->auto_next_stable_hits >= SDCARD_AUTO_NEXT_DRAIN_STABLE_HITS);
    drain_timeout = (filled < 0) ||
                    (elapsed >= pdMS_TO_TICKS(SDCARD_AUTO_NEXT_DRAIN_TIMEOUT_MS));

    if (drain_ready || drain_timeout) {
        if (drain_timeout) {
            ESP_LOGW(TAG,
                     "[auto_next] drain timeout, force flush filled=%d rb=%d elapsed=%lu",
                     filled,
                     rb_size,
                     (unsigned long)pdTICKS_TO_MS(elapsed));
            _flush_pause_tail_pcm();
        } else {
            ESP_LOGI(TAG,
                     "[auto_next] drain ready filled=%d rb=%d hits=%u elapsed=%lu",
                     filled,
                     rb_size,
                     (unsigned int)s_sdcard_handler->auto_next_stable_hits,
                     (unsigned long)pdTICKS_TO_MS(elapsed));
        }

        s_sdcard_handler->auto_next_pending = false;
        s_sdcard_handler->auto_next_wait_tick = 0;
        s_sdcard_handler->auto_next_stable_hits = 0;
        _on_next_song_force();
        return true;
    }

    app_player_set_base_stream_read_freeze(false);
    app_player_set_base_stream_silence_wait(true);
    (void)_schedule_loading_state_check(SDCARD_AUTO_NEXT_DRAIN_CHECK_MS, "auto_next_drain");
    return true;
}

/*
 * 手动切歌软切换：
 * 1. 先让混音侧进入静音等待。
 * 2. 只冻结下游消费端并裁掉已暴露的尾音，提升“像自动切歌”一样的听感。
 * 3. 随后再进入热切 stop/reset 或常规 recycle + 新歌预充流程。
 */
static void _prepare_manual_soft_switch(void)
{
    audio_element_state_t pipe_state;

    if (s_sdcard_handler == NULL || s_sdcard_handler->pipeline == NULL) {
        return;
    }

    s_sdcard_handler->soft_switch_mute_active = true;
    s_sdcard_handler->resume_guard_active = false;
    app_player_set_base_stream_read_freeze(true);
    app_player_set_base_stream_silence_wait(true);

    pipe_state = audio_pipeline_get_state(s_sdcard_handler->pipeline);
    ESP_LOGI(TAG, "[soft_switch] mute prepared, pipe_state=%d", pipe_state);
}

/*
 * 播放前按曲目格式调整任务优先级（不改内存，不改管道）：
 * 1. 首次记录 fatfs/decoder/filter 当前优先级作为基线。
 * 2. WAV：确保 fatfs 高于 decoder/filter 这些消费端任务。
 * 3. MP3/其他：恢复到基线优先级。
 */
static esp_err_t _apply_track_prio_policy(const track_play_plan_t *plan)
{
    sdcard_prio_policy_t target_policy;
    UBaseType_t fatfs_target_prio = 0;
    UBaseType_t decoder_target_prio = SDCARD_DECODER_TASK_PRIO_DEFAULT;
    UBaseType_t fatfs_baseline = 0;
    UBaseType_t decoder_baseline = 0;
    UBaseType_t filter_baseline = 0;
    UBaseType_t filter_target_prio = 0;
    bool fatfs_set_ok = false;
    bool decoder_set_ok = false;
    bool filter_prio_ok = false;

    if (s_sdcard_handler == NULL || plan == NULL || s_sdcard_handler->pipeline == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * 任务句柄仅在 pipeline run 后可获取，这里首次抓取基线。
     */
    if (!s_sdcard_handler->task_prio_baseline_valid) {
        if (_get_task_prio_by_name("sdcard_fatfs", &fatfs_baseline) &&
            _get_task_prio_by_name("sdcard_decoder", &decoder_baseline) &&
            _get_task_prio_by_name("sdcard_filter", &filter_baseline)) {
            s_sdcard_handler->fatfs_task_prio_baseline = fatfs_baseline;
            s_sdcard_handler->decoder_task_prio_baseline = decoder_baseline;
            s_sdcard_handler->filter_task_prio_baseline = filter_baseline;
            s_sdcard_handler->task_prio_baseline_valid = true;
        }
    }

    if (s_sdcard_handler->task_prio_baseline_valid) {
        fatfs_target_prio = s_sdcard_handler->fatfs_task_prio_baseline;
        decoder_target_prio = s_sdcard_handler->decoder_task_prio_baseline;
        filter_target_prio = s_sdcard_handler->filter_task_prio_baseline;
    } else {
        /*
         * 基线获取失败时回退到现有 decoder 配置，保证逻辑可运行。
         */
        fatfs_target_prio = decoder_target_prio;
        filter_target_prio = decoder_target_prio;
    }

    filter_prio_ok = _get_task_prio_by_name("sdcard_filter", &filter_baseline);
    target_policy = _get_track_prio_policy(plan);
    if (target_policy == SDCARD_PRIO_POLICY_WAV_PRODUCER_FIRST) {
        UBaseType_t highest_consumer_prio = decoder_target_prio;
        UBaseType_t wav_fatfs_prio = 0;

        if (filter_prio_ok && filter_baseline > highest_consumer_prio) {
            highest_consumer_prio = filter_baseline;
        } else if (filter_target_prio > highest_consumer_prio) {
            highest_consumer_prio = filter_target_prio;
        }

        wav_fatfs_prio = highest_consumer_prio + 1;
        if (wav_fatfs_prio > (UBaseType_t)(configMAX_PRIORITIES - 1)) {
            wav_fatfs_prio = (UBaseType_t)(configMAX_PRIORITIES - 1);
        }
        if (wav_fatfs_prio < SDCARD_WAV_FATFS_TASK_PRIO) {
            wav_fatfs_prio = SDCARD_WAV_FATFS_TASK_PRIO;
        }
        fatfs_target_prio = wav_fatfs_prio;
    }

    _log_track_task_prio("before_apply");
    ESP_LOGI(TAG,
             "[track_prio] policy=%s wav=%d fatfs_base=%u decoder_base=%u filter_ok=%d filter_base=%u fatfs_target=%u decoder_target=%u",
             target_policy == SDCARD_PRIO_POLICY_WAV_PRODUCER_FIRST ? "wav_producer_first" : "default",
             plan->is_wav_track ? 1 : 0,
             (unsigned int)s_sdcard_handler->fatfs_task_prio_baseline,
             (unsigned int)s_sdcard_handler->decoder_task_prio_baseline,
             filter_prio_ok ? 1 : 0,
             (unsigned int)filter_baseline,
             (unsigned int)fatfs_target_prio,
             (unsigned int)decoder_target_prio);

    fatfs_set_ok = _set_task_prio_by_name("sdcard_fatfs", fatfs_target_prio);
    decoder_set_ok = _set_task_prio_by_name("sdcard_decoder", decoder_target_prio);

    if (!fatfs_set_ok || !decoder_set_ok) {
        ESP_LOGW(TAG, "[track_prio] apply partial, fatfs_ok=%d decoder_ok=%d",
                 fatfs_set_ok ? 1 : 0,
                 decoder_set_ok ? 1 : 0);
    }
    _log_track_task_prio("after_apply");
    return ESP_OK;
}

static esp_err_t _start_pipeline_for_plan(const track_play_plan_t *plan)
{
    float preload_threshold = 0.0f;
    TickType_t resume_timeout = pdMS_TO_TICKS(SDCARD_STARTUP_RESUME_TIMEOUT_MS);
    esp_err_t ret = ESP_OK;

    if (s_sdcard_handler == NULL || s_sdcard_handler->pipeline == NULL || plan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 起播前统一设置 byte_pos，保证首播/停止后重播都从合法音频位置进入解码器。 */
    audio_element_set_byte_pos(s_sdcard_handler->element_fatfs, (int)plan->start_offset);

    if ((AEL_STATE_INIT == audio_element_get_state(s_sdcard_handler->element_fatfs))
        || (AEL_STATE_STOPPED == audio_element_get_state(s_sdcard_handler->element_fatfs))
        || (AEL_STATE_FINISHED == audio_element_get_state(s_sdcard_handler->element_fatfs))
        || (AEL_STATE_ERROR == audio_element_get_state(s_sdcard_handler->element_fatfs))) {
        audio_element_run(s_sdcard_handler->element_fatfs);
    }
    if ((AEL_STATE_INIT == audio_element_get_state(s_sdcard_handler->element_decoder))
        || (AEL_STATE_STOPPED == audio_element_get_state(s_sdcard_handler->element_decoder))
        || (AEL_STATE_FINISHED == audio_element_get_state(s_sdcard_handler->element_decoder))
        || (AEL_STATE_ERROR == audio_element_get_state(s_sdcard_handler->element_decoder))) {
        audio_element_run(s_sdcard_handler->element_decoder);
    }
    if ((AEL_STATE_INIT == audio_element_get_state(s_sdcard_handler->element_rsp_filter))
        || (AEL_STATE_STOPPED == audio_element_get_state(s_sdcard_handler->element_rsp_filter))
        || (AEL_STATE_FINISHED == audio_element_get_state(s_sdcard_handler->element_rsp_filter))
        || (AEL_STATE_ERROR == audio_element_get_state(s_sdcard_handler->element_rsp_filter))) {
        audio_element_run(s_sdcard_handler->element_rsp_filter);
    }
    // do not run element_raw, to prevent it from consuming data from output_rb
    // if ((AEL_STATE_INIT == audio_element_get_state(s_sdcard_handler->element_raw))
    //     || (AEL_STATE_STOPPED == audio_element_get_state(s_sdcard_handler->element_raw))
    //     || (AEL_STATE_FINISHED == audio_element_get_state(s_sdcard_handler->element_raw))
    //     || (AEL_STATE_ERROR == audio_element_get_state(s_sdcard_handler->element_raw))) {
    //     audio_element_run(s_sdcard_handler->element_raw);
    // }

    vTaskDelay(1);

    if (ESP_OK != _apply_track_prio_policy(plan)) {
        ESP_LOGW(TAG, "[start_pipeline] apply track priority policy failed");
    }

    // We have startup_guard to wait for the final output_rb to fill up.
    // Do NOT use ADF's internal preload_threshold here, because it waits for the element's INPUT ringbuffer,
    // which will be constantly drained by the element itself and cause a 2000ms timeout!
    preload_threshold = 0.0f;
    ret |= audio_element_resume(s_sdcard_handler->element_fatfs, 0.0f, resume_timeout);
    ret |= audio_element_resume(s_sdcard_handler->element_decoder, preload_threshold, resume_timeout);
    ret |= audio_element_resume(s_sdcard_handler->element_rsp_filter, 0.0f, resume_timeout);
    // ret |= audio_element_resume(s_sdcard_handler->element_raw, 0.0f, resume_timeout);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[start_preload] resume failed, wav=%d threshold=%.2f ret=%d",
                 plan->is_wav_track ? 1 : 0,
                 (double)preload_threshold,
                 ret);
        audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_ERROR);
        audio_pipeline_terminate(s_sdcard_handler->pipeline);
        return ESP_FAIL;
    }

    audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_RUNNING);
    ESP_LOGI(TAG, "[start_preload] wav=%d threshold=%.2f",
             plan->is_wav_track ? 1 : 0,
             (double)preload_threshold);
    return ESP_OK;
}

static esp_err_t _build_current_track_plan(track_play_plan_t *plan)
{
    if (plan == NULL || s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(plan, 0, sizeof(*plan));
    if (s_sdcard_handler->current_playlist_url[0] == '\0' &&
        s_sdcard_handler->current_playback_url[0] == '\0') {
        return ESP_FAIL;
    }

    if (s_sdcard_handler->current_playlist_url[0] != '\0') {
        plan->playlist_url = s_sdcard_handler->current_playlist_url;
    } else {
        plan->playlist_url = s_sdcard_handler->current_playback_url;
    }
    snprintf(plan->playback_url, sizeof(plan->playback_url), "%s", s_sdcard_handler->current_playback_url);
    plan->start_offset = s_sdcard_handler->current_track_start_offset;
    plan->format = _classify_track_format(plan->playlist_url);
    plan->is_wav_track = _is_wav_url(plan->playlist_url);
    plan->use_shadow_file = (strcmp(plan->playback_url, SDCARD_WAV_SHADOW_PATH) == 0);
    memset(&plan->mp3_info, 0, sizeof(plan->mp3_info));
    memset(&plan->wav_info, 0, sizeof(plan->wav_info));
    return ESP_OK;
}

static bool _get_task_prio_by_name(const char *task_name, UBaseType_t *out_prio)
{
    TaskHandle_t task_handle = NULL;

    if (task_name == NULL || out_prio == NULL) {
        return false;
    }

    task_handle = xTaskGetHandle(task_name);
    if (task_handle == NULL) {
        return false;
    }

    *out_prio = uxTaskPriorityGet(task_handle);
    return true;
}

static bool _set_task_prio_by_name(const char *task_name, UBaseType_t target_prio)
{
    TaskHandle_t task_handle = NULL;
    UBaseType_t current_prio = 0;

    if (task_name == NULL) {
        return false;
    }

    task_handle = xTaskGetHandle(task_name);
    if (task_handle == NULL) {
        ESP_LOGW(TAG, "[track_prio] task not found: %s", task_name);
        return false;
    }

    current_prio = uxTaskPriorityGet(task_handle);
    if (current_prio != target_prio) {
        vTaskPrioritySet(task_handle, target_prio);
        ESP_LOGI(TAG, "[track_prio] %s %u->%u",
                 task_name,
                 (unsigned int)current_prio,
                 (unsigned int)target_prio);
    }
    return true;
}

static void _log_track_task_prio(const char *stage)
{
    UBaseType_t fatfs_prio = 0;
    UBaseType_t decoder_prio = 0;
    UBaseType_t filter_prio = 0;
    bool fatfs_ok = false;
    bool decoder_ok = false;
    bool filter_ok = false;

    fatfs_ok = _get_task_prio_by_name("sdcard_fatfs", &fatfs_prio);
    decoder_ok = _get_task_prio_by_name("sdcard_decoder", &decoder_prio);
    filter_ok = _get_task_prio_by_name("sdcard_filter", &filter_prio);

    ESP_LOGI(TAG,
             "[track_prio][%s] fatfs_ok=%d fatfs=%u decoder_ok=%d decoder=%u filter_ok=%d filter=%u",
             stage ? stage : "null",
             fatfs_ok ? 1 : 0,
             (unsigned int)fatfs_prio,
             decoder_ok ? 1 : 0,
             (unsigned int)decoder_prio,
             filter_ok ? 1 : 0,
             (unsigned int)filter_prio);
}

static void set_state(sdcard_state_t state)
{
    s_sdcard_handler->state = state;
    if (state == SD_PLAYER_STATE_LOADING)
    {
        s_sdcard_handler->loading_enter_tick = xTaskGetTickCount();
        (void)_schedule_loading_state_check(300, "set_state_loading");
    }
    if (state == SD_PLAYER_STATE_PLAYING)
    {
        s_sdcard_handler->current_track_play_tick = xTaskGetTickCount();
        s_sdcard_handler->loading_enter_tick = 0;
        s_sdcard_handler->auto_next_pending = false;
        s_sdcard_handler->auto_next_wait_tick = 0;
        s_sdcard_handler->auto_next_stable_hits = 0;
        if (!s_sdcard_handler->startup_guard_active) {
            s_sdcard_handler->soft_switch_mute_active = false;
            s_sdcard_handler->startup_stable_hits = 0;
        }
    }
    else if (state != SD_PLAYER_STATE_SWITCHING && state != SD_PLAYER_STATE_LOADING)
    {
        if (!s_sdcard_handler->pause_transition_active) {
            app_player_set_base_stream_read_freeze(false);
            app_player_set_base_stream_silence_wait(false);
        }
        app_player_set_base_stream_low_water_duck(false, 0);
        s_sdcard_handler->soft_switch_mute_active = false;
        s_sdcard_handler->startup_guard_active = false;
        s_sdcard_handler->startup_stable_hits = 0;
        s_sdcard_handler->auto_next_pending = false;
        s_sdcard_handler->auto_next_wait_tick = 0;
        s_sdcard_handler->auto_next_stable_hits = 0;
        s_sdcard_handler->low_water_guard_active = false;
        s_sdcard_handler->low_water_enter_hits = 0;
        s_sdcard_handler->low_water_recover_hits = 0;
        s_sdcard_handler->low_water_duck_state = LOW_WATER_DUCK_STATE_IDLE;
        s_sdcard_handler->low_water_enter_tick = 0;
        s_sdcard_handler->low_water_saved_volume = 0;
        s_sdcard_handler->low_water_duck_volume = 0;
        s_sdcard_handler->loading_enter_tick = 0;
    }

    ESP_LOGI(TAG, "[set_state] to: %s", state_log_str[state]);
}
void app_player_sdcard_print_state()
{
    if (s_sdcard_handler && s_sdcard_handler->pipeline)
    {
        ESP_LOGW(TAG, "sdcard[%4x].pipeline:%d,fatfs:%d,decoder:%d,rsp_filter:%d,raw(bypass):%d",
                 (unsigned int)s_sdcard_handler->pipeline,
                 s_sdcard_handler->pipeline ? audio_pipeline_get_state(s_sdcard_handler->pipeline) : -1,
                 s_sdcard_handler->element_fatfs ? audio_element_get_state(s_sdcard_handler->element_fatfs) : -1,
                 s_sdcard_handler->element_decoder ? audio_element_get_state(s_sdcard_handler->element_decoder) : -1,
                 s_sdcard_handler->element_rsp_filter ? audio_element_get_state(s_sdcard_handler->element_rsp_filter) : -1,
                 s_sdcard_handler->element_raw ? audio_element_get_state(s_sdcard_handler->element_raw) : -1);
    }
}

/*
 * DEBUG: _play_latest - internal implementation
 * This function contains the actual logic for playing the current playlist item.
 * It is called by manual switch, auto next, and loading retry paths.
 */
static void _play_latest()
{
    track_play_plan_t plan;
    bool use_hot_reset = false;

    if (s_sdcard_handler->state == SD_PLAYER_STATE_PLAYING)
    {
        ESP_LOGI(TAG, "[_play_latest] SD_PLAYER_STATE_PLAYING");
    }
    else
    {
        ESP_LOGI(TAG, "[_play_latest] SD_PLAYER_STATE not playing");
    }
    set_state(SD_PLAYER_STATE_SWITCHING);

    /*
     * 最小改动策略：
     * 1. MP3->MP3、WAV->WAV 走热路径，减少 terminate 带来的切歌等待。
     * 2. MP3<->WAV 或其他格式组合继续走 recycle，保持跨格式稳定性。
     */
    use_hot_reset = _should_use_hot_reset_for_current_track();
    ESP_LOGD(TAG,
             "[track_switch] mode=%s current_format=%d next_url=%s current_url=%s",
             use_hot_reset ? "hot_reset" : "recycle",
             (int)s_sdcard_handler->current_track_format,
             s_sdcard_handler->current_playlist_url[0] != '\0' ? s_sdcard_handler->current_playlist_url : "none",
             s_sdcard_handler->prev_playlist_url[0] != '\0' ? s_sdcard_handler->prev_playlist_url : "none");
    if (use_hot_reset) {
        _hot_reset_pipeline_for_track_switch();
    } else {
        _recycle_pipeline_for_track_switch();
    }

    if (ESP_OK != _select_playable_track(&plan))
    {
        ESP_LOGE(TAG, "[_play_latest] can not get playable url");
        _log_diag_stats("no_playable_url");
        _clear_current_track_context();
        set_state(SD_PLAYER_STATE_STOPPED);
        return;
    }
    // 曲目号在 LOADING 稳定后统一刷新，避免启动失败时 UI 已经跳到下一首。

    _remember_current_track(&plan);
    app_player_sdcard_refresh_track_display();
    vTaskDelay(pdMS_TO_TICKS(50));

    // 稳妥赋值：在状态彻底干净后，设置新歌路径和指针归零
    audio_element_set_uri(s_sdcard_handler->element_fatfs, plan.playback_url);
    // MP3 预检通过后，直接从首个合法音频帧起播，避免脏头被误识别成 PCM。
    audio_element_set_byte_pos(s_sdcard_handler->element_fatfs, (int)plan.start_offset);

    vTaskDelay(10 / portTICK_PERIOD_MS);
    if (ESP_OK != _start_pipeline_for_plan(&plan))
    {
        ESP_LOGE(TAG, "[_play_latest] start pipeline failed");
        set_state(SD_PLAYER_STATE_STOPPED);
        return;
    }
    set_state(SD_PLAYER_STATE_LOADING);
    ESP_LOGW(TAG,
             "[_play_latest] playlist:%s playback:%s start_offset=%lu shadow:%d",
             plan.playlist_url,
             plan.playback_url,
             (unsigned long)plan.start_offset,
             plan.use_shadow_file ? 1 : 0);
}

static void switch_commit_timer_callback(U16 timerId, void *arg)
{
    (void)timerId;
    (void)arg;
    send_cmd(SDCARD_CMD_SWITCH_COMMIT);
}

static void _request_switch_step(int32_t step, const char *source)
{
    if (s_sdcard_handler == NULL || step == 0) {
        return;
    }

    if (step > 0) {
        s_sdcard_handler->stats.next_request_count++;
    } else {
        s_sdcard_handler->stats.prev_request_count++;
    }

    /*
     * 后台扫描期间，上一首依赖完整曲库总数才能正确从 001 回绕到最后一首。
     * 若此时仍使用旧缓存/半旧总数，容易出现 001 -> 200 这类错误回绕。
     * 因此在扫描完成前只屏蔽上一首，扫描结束后再允许正常切换。
     */
    if (step < 0 && s_sdcard_handler->scan_task_running) {
        s_sdcard_handler->stats.prev_drop_count++;
        ESP_LOGW(TAG,
                 "[scan_guard] prev locked while scan running total=%lu source=%s",
                 (unsigned long)s_sdcard_handler->track_total,
                 source ? source : "unknown");
        return;
    }

    // 先尝试修正“状态卡在 LOADING，但底层管线已进入稳定态”的异常，避免按键被永久锁死。
    if (s_sdcard_handler->state == SD_PLAYER_STATE_LOADING &&
        !s_sdcard_handler->switch_debounce_active) {
        (void)_recover_stale_loading_state(source);
    }

    // [最小改动修复]: 如果底层管线正处于重建/加载状态(LOADING)，或者已经被标记为切换保护中，
    // 我们必须坚决丢弃此时的按键事件，防止打断脆弱的初始化过程导致死机。
    if (s_sdcard_handler->state == SD_PLAYER_STATE_LOADING || 
        s_sdcard_handler->state == SD_PLAYER_STATE_SWITCHING) {
        if (!s_sdcard_handler->switch_debounce_active) {
            ESP_LOGW(TAG, "[switch_debounce] pipeline busy (state=%d), dropping key step=%ld",
                     s_sdcard_handler->state, (long)step);
            if (step > 0) s_sdcard_handler->stats.next_drop_count++;
            else s_sdcard_handler->stats.prev_drop_count++;
            return;
        }
    }

    // 累加切歌步数并重置防抖窗口
    s_sdcard_handler->switch_debounce_active = true;
    s_sdcard_handler->pending_switch_steps += step;
    set_state(SD_PLAYER_STATE_SWITCHING);

    // FW_SetTimer 内部自带相同 timerId 的复用和自动重置(xTimerChangePeriod)机制。
    // 因此在 200ms 内反复按键时，这里会自动重新开始 200ms 倒计时，无需手动 Release。
    if (FW_SetTimer(switch_commit_timer_callback,
                    SDCARD_TIMER_ID_SWITCH_COMMIT,
                    NULL,
                    SDCARD_SWITCH_DEBOUNCE_MS) != pdPASS) {
        ESP_LOGW(TAG, "[switch_debounce] arm timer failed, source=%s step=%ld",
                 source ? source : "unknown",
                 (long)step);
        _commit_pending_switch();
        return;
    }

    ESP_LOGI(TAG,
             "[switch_debounce] source=%s step=%ld pending=%ld wait=%ums",
             source ? source : "unknown",
             (long)step,
             (long)s_sdcard_handler->pending_switch_steps,
             SDCARD_SWITCH_DEBOUNCE_MS);
}

static void _apply_playlist_offset(int32_t step)
{
    int32_t next_index = 0;
    int32_t total = 0;

    if (s_sdcard_handler == NULL || step == 0) {
        return;
    }
    if (s_sdcard_handler->track_total == 0U) {
        return;
    }

    total = (int32_t)s_sdcard_handler->track_total;
    next_index = (int32_t)s_sdcard_handler->track_index + step;
    /* 上一曲不回绕，clamp 到第 1 首(index 0)，防止歌曲过多时回绕出错 */
    if (next_index < 0) {
        next_index = 0;
    }
    while (next_index >= total) {
        next_index -= total;
    }

    _cache_set_current_index((uint32_t)next_index);
}

static bool _should_use_hot_reset_for_current_track(void)
{
    track_format_t next_format;

#if !SDCARD_HOT_SWITCH_ON_SAME_TYPE
    ESP_LOGI(TAG, "[#track_switch] same type hot switch disabled by macro, rebuild pipeline by default");
    return false;
#endif

    if (s_sdcard_handler == NULL || s_sdcard_handler->current_playlist_url[0] == '\0') {
        ESP_LOGI(TAG, "[track_switch] hot reset disabled: next url missing");
        return false;
    }
    if (!(s_sdcard_handler->current_track_format == TRACK_FORMAT_MP3 ||
          s_sdcard_handler->current_track_format == TRACK_FORMAT_WAV)) {
        ESP_LOGD(TAG,
                 "[track_switch] hot reset disabled: current format unsupported=%d url=%s",
                 (int)s_sdcard_handler->current_track_format,
                 s_sdcard_handler->current_playlist_url);
        return false;
    }

    next_format = _classify_track_format(s_sdcard_handler->current_playlist_url);
    if (next_format != s_sdcard_handler->current_track_format) {
        ESP_LOGD(TAG,
                 "[track_switch] cross format recycle current=%d next=%d url=%s",
                 (int)s_sdcard_handler->current_track_format,
                 (int)next_format,
                 s_sdcard_handler->current_playlist_url);
        return false;
    }

    ESP_LOGD(TAG,
             "[track_switch] same format hot reset current=%d next=%d url=%s",
             (int)s_sdcard_handler->current_track_format,
             (int)next_format,
             s_sdcard_handler->current_playlist_url);
    return true;
}

static void _commit_pending_switch(void)
{
    int32_t pending_steps = 0;
    audio_element_state_t pipe_state;

    if (s_sdcard_handler == NULL || !s_sdcard_handler->switch_debounce_active) {
        return;
    }

    pending_steps = s_sdcard_handler->pending_switch_steps;
    s_sdcard_handler->pending_switch_steps = 0;
    s_sdcard_handler->switch_debounce_active = false;

    if (_apply_pending_cache_if_needed() != ESP_OK) {
        ESP_LOGW(TAG, "[cache] apply pending cache before switch failed");
    }

    if (pending_steps == 0) {
        pipe_state = audio_pipeline_get_state(s_sdcard_handler->pipeline);
        if (pipe_state == AEL_STATE_RUNNING) {
            set_state(SD_PLAYER_STATE_PLAYING);
        } else if (pipe_state == AEL_STATE_PAUSED) {
            set_state(SD_PLAYER_STATE_PAUSED);
        }
        ESP_LOGI(TAG, "[switch_debounce] pending step canceled");
        return;
    }

    /* 预计算目标索引，若索引未改变（如 index 0 处按上一曲），直接跳过切换并继续当前播放 */
    int32_t current_index = (int32_t)s_sdcard_handler->track_index;
    int32_t total = (int32_t)s_sdcard_handler->track_total;
    int32_t next_index = current_index + pending_steps;
    if (next_index < 0) {
        next_index = 0;
    }
    if (total > 0) {
        while (next_index >= total) {
            next_index -= total;
        }
    }

    if (next_index == current_index) {
        pipe_state = audio_pipeline_get_state(s_sdcard_handler->pipeline);
        if (pipe_state == AEL_STATE_RUNNING) {
            set_state(SD_PLAYER_STATE_PLAYING);
        } else if (pipe_state == AEL_STATE_PAUSED) {
            set_state(SD_PLAYER_STATE_PAUSED);
        } else {
            /*
             * 开机未播放且位于第1首时，按上一曲会命中“索引未变化”分支。
             * 此时若不显式恢复状态，控制层会残留在 SWITCHING，导致后续播放/切歌全部失效。
             */
            set_state(SD_PLAYER_STATE_STOPPED);
        }
        ESP_LOGI(TAG,
                 "[switch_debounce] index unchanged (%ld -> %ld), restore state from pipe=%d and skip switch",
                 (long)current_index,
                 (long)next_index,
                 pipe_state);
        return;
    }

    ESP_LOGI(TAG, "[switch_debounce] commit steps=%ld", (long)pending_steps);
    _prepare_manual_soft_switch();
    _apply_playlist_offset(pending_steps);
    _play_latest();
}

/*
 * DEBUG: _on_next_song - internal implementation
 * Advances to the next song in the playlist.
 * Called from sdcard_cmd_task when SDCARD_CMD_NEXT is received.
 */
static void _on_next_song()
{
    _request_switch_step(1, "_on_next_song");
}

static void _on_next_song_force(void)
{
    ESP_LOGI(TAG, "[_on_next_song_force] immediate next");
    if (_apply_pending_cache_if_needed() != ESP_OK) {
        ESP_LOGW(TAG, "[cache] apply pending cache before force next failed");
    }
    _prepare_manual_soft_switch();
    set_state(SD_PLAYER_STATE_SWITCHING);
    _apply_playlist_offset(1);
    _play_latest();
}

/*
 * DEBUG: _on_prev_song - internal implementation
 * Goes to the previous song in the playlist.
 * Called from sdcard_cmd_task when SDCARD_CMD_PREV is received.
 */
static void _on_prev_song()
{
    _request_switch_step(-1, "_on_prev_song");
}

static void sdcard_event_handler(void *pv)
{
    audio_event_iface_handle_t evt = (audio_event_iface_handle_t )pv;
    while (1)
    {
        /* Handle event interface messages from pipeline
           to set music info and to advance to the next song
        */
        audio_event_iface_msg_t msg;
        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "[sdcard_event_handler] Event interface error : %d", ret);
            continue;
        }

        ESP_LOGI(TAG, "[sdcard_event_handler] evt receive [%s], cmd=%d, data=%d", audio_element_get_tag(msg.source), msg.cmd, (int)msg.data);
        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT)
        {
            // Advance to the next song when previous finishes
            if (msg.source == (void *)(s_sdcard_handler->element_rsp_filter) && msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
            {
                audio_element_state_t el_state = audio_element_get_state(s_sdcard_handler->element_rsp_filter);
                ESP_LOGD(TAG, "[sdcard_event_handler] rsp_filte state: %d", el_state);
                // if(s_player->current_base_pipeline!= s_player->sdcard_audio.pipeline){
                //     continue;
                // }
                // else

                // 播放完成
                if (el_state == AEL_STATE_FINISHED)
                {
                    ESP_LOGW(TAG, "[sdcard_event_handler] Music Finished");
                    if (s_sdcard_handler->state == SD_PLAYER_STATE_PLAYING)
                    {
                        _reset_low_water_guard_state(true, true);
                        s_sdcard_handler->auto_next_pending = true;
                        s_sdcard_handler->auto_next_wait_tick = xTaskGetTickCount();
                        s_sdcard_handler->auto_next_stable_hits = 0;
                        set_state(SD_PLAYER_STATE_SWITCHING);
                        app_player_set_base_stream_read_freeze(false);
                        app_player_set_base_stream_silence_wait(true);
                        (void)_schedule_loading_state_check(SDCARD_AUTO_NEXT_DRAIN_CHECK_MS, "music_finished");
                        ESP_LOGI(TAG, "[auto_next] wait drain after finished");
                        // ESP_LOGI(TAG, "[sdcard_event_handler] SDcard music next");
                        // char *url = NULL;
                        // sdcard_list_next(s_sdcard_handler->sdcard_list, 1, &url);
                        // ESP_LOGI(TAG, "[sdcard_event_handler] URL: %s", url);
                        /* In previous versions, audio_pipeline_terminal() was called here. It will close all the element task and when we use
                         * the pipeline next time, all the tasks should be restarted again. It wastes too much time when we switch to another music.
                         * So we use another method to achieve this as below.
                         */
                        // audio_element_set_uri(s_sdcard_handler->element_fatfs, url);
                        // audio_pipeline_reset_ringbuffer(s_sdcard_handler->pipeline);
                        // audio_pipeline_reset_elements(s_sdcard_handler->pipeline);
                        // audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_INIT);
                        // // audio_pipeline_resume(s_player->mixer.pipeline);
                        // audio_pipeline_run(s_sdcard_handler->pipeline);
                    }
                }
            }
            // 收到音乐信息， 设置滤波器参数
            if (msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO)
            {
                audio_element_info_t music_info = {0};
                audio_element_getinfo(msg.source, &music_info);
                ESP_LOGI(TAG, "[sdcard_event_handler] Received music info from [%s], sample_rates=%d, bits=%d, ch=%d ",
                         audio_element_get_tag(msg.source), music_info.sample_rates, music_info.bits, music_info.channels);

                if (msg.source == (void *)s_sdcard_handler->element_decoder)
                {
                    rsp_filter_set_src_info(s_sdcard_handler->element_rsp_filter, music_info.sample_rates, music_info.channels);
                }

                if (s_sdcard_handler->state == SD_PLAYER_STATE_LOADING) {
                    (void)_schedule_loading_state_check(30, "music_info");
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
            if ((msg.source == (void *)s_sdcard_handler->element_fatfs))
            {
                if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
                {
                    audio_element_status_t el_state = (int)msg.data;
                    if (s_sdcard_handler->state == SD_PLAYER_STATE_LOADING &&
                        el_state == AEL_STATUS_STATE_RUNNING) {
                        (void)_schedule_loading_state_check(30, "fatfs_running");
                    }
                    if (el_state == AEL_STATUS_STATE_RUNNING)
                    {
                        // 这里需要发送消息给主player
                        // board_display_set_play_state(MX_PLAY_PLAY);
                    }
                    else
                    {
                        // 这里需要发送消息给主player
                        // board_display_set_play_state(MX_PLAY_PAUSE);
                    }
                }
            }
            // Update song number
            if (msg.source == (void *)s_sdcard_handler->element_fatfs)
            {
                if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS)
                {
                    audio_element_status_t el_state = (int)msg.data;
                    if (el_state == AEL_STATUS_STATE_RUNNING)
                    {
                        // uint16_t id = sdcard_list_get_url_id(s_sdcard_handler->sdcard_list);
                        // // 这里需要发送消息给主player
                        // displayer_track_num(id);
                    }
                    if (el_state == AEL_STATUS_ERROR_INPUT)
                    {
                        ESP_LOGE(TAG, "[sdcard_event_handler] element_fatfs AEL_STATUS_ERROR_INPUT");
                        
                        // 增加状态屏蔽防抖：只有在非 LOADING/SWITCHING 状态（如正常播放中拔卡）才立即切歌
                        // LOADING/SWITCHING 状态下的错误忽略，交由 _play_state_check_impl 的重试机制处理
                        if (s_sdcard_handler->state != SD_PLAYER_STATE_LOADING && s_sdcard_handler->state != SD_PLAYER_STATE_SWITCHING) {
                            ESP_LOGI(TAG, "on_next_song called: AEL_STATUS_ERROR_INPUT (Not Loading/Switching)");
                            on_next_song_force();
                        } else {
                            ESP_LOGW(TAG, "Ignore ERROR_INPUT during LOADING/SWITCHING, wait for timer retry");
                        }
                    }
                }
            }
            // audio element err
            if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS && (int)(msg.data) < AEL_STATUS_ERROR_UNKNOWN)
            {
                ESP_LOGE(TAG, "[sdcard_event_handler] audio element err");
                if (msg.source == s_sdcard_handler->element_decoder)
                {
                    s_sdcard_handler->stats.decoder_error_count++;
                    ESP_LOGE(TAG, "[sdcard_event_handler] sdcard_audio.element_decoder err,  %d", (int)msg.data);
                    _log_diag_stats("decoder_err");
                    // app_player_on_next();
                }
                if (msg.source == s_sdcard_handler->element_fatfs)
                {
                    ESP_LOGE(TAG, "[sdcard_event_handler] sdcard_audio.element_fatfs err,  %d", (int)msg.data);
                    // app_player_on_next();
                }
                if (msg.source == s_sdcard_handler->element_raw)
                {
                    ESP_LOGE(TAG, "[sdcard_event_handler] sdcard_audio.element_raw err, %d", (int)msg.data);
                    // app_player_on_next();
                }
                if (msg.source == s_sdcard_handler->element_rsp_filter)
                {
                    ESP_LOGE(TAG, "[sdcard_event_handler] sdcard_audio.element_rsp_filter err, %d", (int)msg.data);
                    // app_player_on_next();
                }
            }

            if (msg.cmd == AEL_MSG_CMD_REPORT_STATUS &&
                (int)msg.data == AEL_STATUS_STATE_RUNNING &&
                s_sdcard_handler->state == SD_PLAYER_STATE_LOADING &&
                (msg.source == s_sdcard_handler->element_decoder ||
                 msg.source == s_sdcard_handler->element_rsp_filter)) {
                (void)_schedule_loading_state_check(30, "element_running");
            }
        }
    }
}

static void sdcard_url_save_cb(void *user_data, char *url)
{
    playlist_handle_t playlist = (playlist_handle_t)user_data;
    esp_err_t ret = playlist_save(playlist, url);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Fail to save sdcard url to sdcard playlist");
    }
}

static void on_sdcard_scan()
{

    ESP_LOGI(TAG, "[scan] rebuild sdcard cache");
    // terminal pipeline
    if (s_sdcard_handler->pipeline != NULL)
    {
        _recycle_pipeline_for_track_switch();
    }

    if (s_sdcard_handler->playlist != NULL)
    {
        playlist_destroy(s_sdcard_handler->playlist);
    }
    remove(SDCARD_WAV_SHADOW_PATH);
    _clear_current_track_context();
    if (_rebuild_track_cache(true) != ESP_OK)
    {
        ESP_LOGE(TAG, "[scan] rebuild cache failed");
        return;
    }

    {
        track_play_plan_t plan;
        if (ESP_OK != _select_playable_track(&plan))
        {
            ESP_LOGE(TAG, "[scan] no playable track after filter");
            return;
        }
        _remember_current_track(&plan);
        audio_element_set_uri(s_sdcard_handler->element_fatfs, plan.playback_url);
        app_player_sdcard_refresh_track_display();
    }
}

static void sdcard_scan_task(void *pv)
{
    bool activate_now = false;

    (void)pv;

    if (s_sdcard_handler != NULL) {
        activate_now = s_sdcard_handler->scan_activate_cache;
    }

    ESP_LOGI(TAG, "[scan_task] start activate=%d", activate_now ? 1 : 0);

    if (activate_now) {
        /*
         * 增量扫描模式：边扫边追加到 active M3U/IDX，扫到第 1 首即开播。
         * 扫描完成后用排序结果（MP3→WAV→Other）重写 active 缓存。
         */
        sdcard_scan_context_t ctx;
        esp_err_t ret = ESP_OK;

        if (_ensure_cache_fs_ready() != ESP_OK) {
            ESP_LOGE(TAG, "[scan_task] cache fs not ready");
            goto scan_done;
        }

        memset(&ctx, 0, sizeof(ctx));
        ctx.limit = SDCARD_SCAN_MAX_TRACKS;
        ctx.signature = 2166136261UL;
        ctx.incremental_mode = true;
        ctx.first_track_notified = false;
        ctx.active_track_count = 0;
        _reset_root_fingerprint(&ctx.root_fp);

        /* 清空旧 active 缓存 */
        remove(SDCARD_CACHE_M3U_PATH);
        remove(SDCARD_CACHE_IDX_PATH);
        remove(SDCARD_CACHE_M3U_TMP_PATH);
        remove(SDCARD_CACHE_IDX_TMP_PATH);
        remove(SDCARD_CACHE_M3U_MP3_TMP_PATH);
        remove(SDCARD_CACHE_M3U_WAV_TMP_PATH);
        remove(SDCARD_CACHE_M3U_OTHER_TMP_PATH);
        remove(SDCARD_WAV_SHADOW_PATH);
        _clear_current_track_context();

        /* 打开 active M3U/IDX 用于增量追加 */
        ctx.active_m3u_fp = fopen(SDCARD_CACHE_M3U_PATH, "wb+");
        ctx.active_idx_fp = fopen(SDCARD_CACHE_IDX_PATH, "wb+");
        if (ctx.active_m3u_fp == NULL || ctx.active_idx_fp == NULL) {
            if (ctx.active_m3u_fp != NULL) fclose(ctx.active_m3u_fp);
            if (ctx.active_idx_fp != NULL) fclose(ctx.active_idx_fp);
            ESP_LOGE(TAG, "[scan_task] create active cache files failed");
            goto scan_done;
        }

        /* 写占位 IDX header */
        {
            sdcard_cache_idx_header_t empty_header;
            memset(&empty_header, 0, sizeof(empty_header));
            fwrite(&empty_header, 1, sizeof(empty_header), ctx.active_idx_fp);
            fflush(ctx.active_idx_fp);
        }

        /* 打开分桶临时文件（用于最终排序重写） */
        ctx.mp3_fp = fopen(SDCARD_CACHE_M3U_MP3_TMP_PATH, "wb+");
        ctx.wav_fp = fopen(SDCARD_CACHE_M3U_WAV_TMP_PATH, "wb+");
        ctx.other_fp = fopen(SDCARD_CACHE_M3U_OTHER_TMP_PATH, "wb+");
        if (ctx.mp3_fp == NULL || ctx.wav_fp == NULL || ctx.other_fp == NULL) {
            if (ctx.mp3_fp != NULL) fclose(ctx.mp3_fp);
            if (ctx.wav_fp != NULL) fclose(ctx.wav_fp);
            if (ctx.other_fp != NULL) fclose(ctx.other_fp);
            fclose(ctx.active_m3u_fp);
            fclose(ctx.active_idx_fp);
            ESP_LOGE(TAG, "[scan_task] create bucket files failed");
            goto scan_done;
        }

        /* 扫描根目录：每首歌同时写入分桶和 active 缓存 */
        ret = _scan_directory_to_cache("/sdcard", &ctx);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "[scan_task] incremental scan failed");
        }

        /* 关闭 active M3U/IDX（后续 finalize 会重写） */
        fclose(ctx.active_m3u_fp);
        fclose(ctx.active_idx_fp);
        ctx.active_m3u_fp = NULL;
        ctx.active_idx_fp = NULL;

        ESP_LOGI(TAG,
                 "[scan_task] incremental scan done mp3=%lu wav=%lu other=%lu total=%lu",
                 (unsigned long)ctx.mp3_count,
                 (unsigned long)ctx.wav_count,
                 (unsigned long)ctx.other_count,
                 (unsigned long)ctx.active_track_count);

        /* 用排序后的结果重写 active 缓存 */
        if (ret == ESP_OK && ctx.active_track_count > 0U) {
            if (_incremental_finalize_cache(&ctx) != ESP_OK) {
                ESP_LOGW(TAG, "[scan_task] finalize sorted cache failed, keep incremental order");
            }
        }

        /* 清理分桶临时文件 */
        fclose(ctx.mp3_fp);
        fclose(ctx.wav_fp);
        fclose(ctx.other_fp);
        remove(SDCARD_CACHE_M3U_MP3_TMP_PATH);
        remove(SDCARD_CACHE_M3U_WAV_TMP_PATH);
        remove(SDCARD_CACHE_M3U_OTHER_TMP_PATH);
    } else if (_rebuild_track_cache(false) != ESP_OK) {
        ESP_LOGE(TAG, "[scan_task] background rebuild failed");
    }

scan_done:
    if (s_sdcard_handler != NULL) {
        s_sdcard_handler->scan_task_handle = NULL;
        s_sdcard_handler->scan_task_running = false;
        s_sdcard_handler->scan_activate_cache = false;
    }

    ESP_LOGI(TAG, "[scan_task] done");
    vTaskDelete(NULL);
}

static esp_err_t _trigger_sdcard_scan_task(bool activate_now)
{
    BaseType_t task_ret = pdFAIL;

    if (s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_sdcard_handler->scan_task_running) {
        ESP_LOGW(TAG, "[scan_task] already running");
        return ESP_OK;
    }

    s_sdcard_handler->scan_task_running = true;
    s_sdcard_handler->scan_activate_cache = activate_now;
    task_ret = xTaskCreatePinnedToCore(
        sdcard_scan_task,
        "sdcard_scan_task",
        SDCARD_SCAN_TASK_STACK_SIZE,
        NULL,
        8,
        &s_sdcard_handler->scan_task_handle,
        1
    );
    if (task_ret != pdPASS)
    {
        s_sdcard_handler->scan_task_handle = NULL;
        s_sdcard_handler->scan_task_running = false;
        s_sdcard_handler->scan_activate_cache = false;
        ESP_LOGE(TAG, "[scan_task] create failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[scan_task] created stack=%u activate=%d",
             (unsigned int)SDCARD_SCAN_TASK_STACK_SIZE,
             activate_now ? 1 : 0);
    return ESP_OK;
}

// void set_play_state_check_timer(audio_element_state_t state){
//     // 设置一个定时器，每隔100ms检查一次播放状态
//     // 如果播放状态发生变化，则更新播放状态
//     // 如果播放状态为停止，则停止定时器
//     // 如果播放状态为播放，则启动定时器
// }

/*
 * DEBUG: _on_play_pause - internal implementation
 * Toggles between play and pause states.
 * Called from sdcard_cmd_task when SDCARD_CMD_PLAY_PAUSE is received.
 */
static void _on_play_pause()
{
    track_play_plan_t plan;

    // 状态机若异常留在 LOADING，这里先按真实管线状态纠偏，避免播放键失效。
    if (s_sdcard_handler->state == SD_PLAYER_STATE_LOADING) {
        (void)_recover_stale_loading_state("_on_play_pause");
    }

    switch (s_sdcard_handler->state)
    {
    case SD_PLAYER_STATE_INIT:
    case SD_PLAYER_STATE_PAUSED:
    case SD_PLAYER_STATE_STOPPED:
    case SD_PLAYER_STATE_PLAYING:
    case SD_PLAYER_STATE_ERROR:
        audio_element_state_t el_state = audio_pipeline_get_state(s_sdcard_handler->pipeline);

        // audio_element_state_t el_state = audio_element_get_state(player->sdcard_audio.element_fatfs);
        ESP_LOGI(TAG, "[_on_play_pause] pipe state: %d", el_state);
        switch (el_state)
        {
        case AEL_STATE_INIT:
            ESP_LOGI(TAG, "[_on_play_pause] Starting audio pipeline");
            if (ESP_OK != _build_current_track_plan(&plan) ||
                ESP_OK != _start_pipeline_for_plan(&plan))
            {
                ESP_LOGE(TAG, "[_on_play_pause] start pipeline failed in init");
                set_state(SD_PLAYER_STATE_STOPPED);
                break;
            }

            // set_play_state_check_timer(AEL_STATE_RUNNING);
            set_state(SD_PLAYER_STATE_LOADING);

            break;
        case AEL_STATE_RUNNING:
            ESP_LOGI(TAG, "[_on_play_pause] Pausing audio pipeline");
            _pause_pipeline_fast();
            set_state(SD_PLAYER_STATE_PAUSED);
            break;
        case AEL_STATE_PAUSED:
            ESP_LOGI(TAG, "[_on_play_pause] Resuming audio pipeline");
            _reset_low_water_guard_state(true, true);
            s_sdcard_handler->resume_guard_active = true;
            s_sdcard_handler->resume_guard_enter_tick = xTaskGetTickCount();
            s_sdcard_handler->startup_guard_active = true;
            s_sdcard_handler->startup_stable_hits = 0;
            app_player_set_base_stream_read_freeze(true);
            app_player_set_base_stream_silence_wait(true);
            if (ESP_OK != audio_pipeline_resume(s_sdcard_handler->pipeline))
            {
                s_sdcard_handler->resume_guard_active = false;
                s_sdcard_handler->resume_guard_enter_tick = 0;
                s_sdcard_handler->startup_guard_active = false;
                app_player_set_base_stream_read_freeze(false);
                app_player_set_base_stream_silence_wait(false);
                ESP_LOGW(TAG, "[_on_play_pause] resume pipeline failed");
                break;
            }
            (void)_schedule_loading_state_check(SDCARD_STARTUP_GUARD_CHECK_MS, "resume_guard");
            break;
        case AEL_STATE_STOPPED:
            // if(ESP_OK != audio_pipeline_resume(s_sdcard_handler->pipeline)){
            //     ESP_LOGE(TAG, "[ * ] Audio pipeline resume filed");
            //     audio_pipeline_stop(s_sdcard_handler->pipeline);
            //     audio_pipeline_wait_for_stop(s_sdcard_handler->pipeline);
            // }
            if (ESP_OK != _build_current_track_plan(&plan) ||
                ESP_OK != _start_pipeline_for_plan(&plan))
            {
                ESP_LOGE(TAG, "[_on_play_pause] start pipeline failed in stopped");
                set_state(SD_PLAYER_STATE_STOPPED);
                break;
            }
            // audio_pipeline_check_items_state(s_sdcard_handler->pipeline, s_sdcard_handler->element_fatfs,AEL_STATE_RUNNING);
            // audio_pipeline_change_state(s_sdcard_handler->pipeline,AEL_STATE_RUNNING);
            // downmix_set_input_rb_timeout(s_player->mixer.element_mixer, 50, 0);
            // set_play_state_check_timer(AEL_STATE_RUNNING);

            set_state(SD_PLAYER_STATE_LOADING);
            break;
        default:
            ESP_LOGE(TAG, "[_on_play_pause]  Not supported state %d, change to init", el_state);
            audio_pipeline_reset_ringbuffer(s_sdcard_handler->pipeline);
            audio_pipeline_reset_elements(s_sdcard_handler->pipeline);
            audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_INIT);
            if (ESP_OK != _build_current_track_plan(&plan) ||
                ESP_OK != _start_pipeline_for_plan(&plan))
            {
                ESP_LOGE(TAG, "[_on_play_pause] start pipeline failed in default");
                set_state(SD_PLAYER_STATE_STOPPED);
                break;
            }
            // set_play_state_check_timer(AEL_STATE_RUNNING);
            set_state(SD_PLAYER_STATE_LOADING);
        }
        break;
    case SD_PLAYER_STATE_LOADING:
    default:
        break;
    }
}

/*
 * 状态纠偏：
 * 某些异常路径下，状态字段可能残留在 LOADING，但实际 pipeline 已经处于 RUNNING/PAUSED/INIT。
 * 此时若继续相信 state，会导致 next/prev/play 键都被 busy 保护永久拦截。
 */
static bool _recover_stale_loading_state(const char *source)
{
    TickType_t now;
    TickType_t elapsed = 0;
    audio_element_state_t pipe_state;
    audio_element_state_t fatfs_state;
    audio_element_state_t decoder_state;
    audio_element_state_t filter_state;
    bool loading_ticket_missing;
    bool loading_stale;

    if (s_sdcard_handler == NULL || s_sdcard_handler->state != SD_PLAYER_STATE_LOADING) {
        return false;
    }

    now = xTaskGetTickCount();
    if (s_sdcard_handler->loading_enter_tick != 0) {
        elapsed = now - s_sdcard_handler->loading_enter_tick;
    }

    loading_ticket_missing = (s_sdcard_handler->loading_enter_tick == 0);
    loading_stale = loading_ticket_missing ||
                    (elapsed >= pdMS_TO_TICKS(SDCARD_LOADING_STALE_RECOVER_MS));
    if (!loading_stale) {
        return false;
    }

    pipe_state = s_sdcard_handler->pipeline ? audio_pipeline_get_state(s_sdcard_handler->pipeline) : AEL_STATE_NONE;
    fatfs_state = s_sdcard_handler->element_fatfs ? audio_element_get_state(s_sdcard_handler->element_fatfs) : AEL_STATE_NONE;
    decoder_state = s_sdcard_handler->element_decoder ? audio_element_get_state(s_sdcard_handler->element_decoder) : AEL_STATE_NONE;
    filter_state = s_sdcard_handler->element_rsp_filter ? audio_element_get_state(s_sdcard_handler->element_rsp_filter) : AEL_STATE_NONE;

    if (pipe_state == AEL_STATE_RUNNING &&
        fatfs_state == AEL_STATE_RUNNING &&
        decoder_state == AEL_STATE_RUNNING &&
        filter_state == AEL_STATE_RUNNING) {
        if (s_sdcard_handler->startup_guard_active ||
            s_sdcard_handler->soft_switch_mute_active) {
            if (_wait_track_startup_buffer_ready()) {
                s_sdcard_handler->startup_guard_active = false;
                s_sdcard_handler->startup_stable_hits = 0;
                s_sdcard_handler->soft_switch_mute_active = false;
                app_player_set_base_stream_read_freeze(false);
                app_player_set_base_stream_silence_wait(false);
                ESP_LOGW(TAG,
                         "[state_recover] force release startup guard, src=%s elapsed=%lu",
                         source ? source : "unknown",
                         (unsigned long)pdTICKS_TO_MS(elapsed));
            }
        }
        ESP_LOGW(TAG,
                 "[state_recover] stale loading -> playing, src=%s elapsed=%lu tick_missing=%d",
                 source ? source : "unknown",
                 (unsigned long)pdTICKS_TO_MS(elapsed),
                 loading_ticket_missing ? 1 : 0);
        set_state(SD_PLAYER_STATE_PLAYING);
        return true;
    }

    if (pipe_state == AEL_STATE_PAUSED) {
        ESP_LOGW(TAG,
                 "[state_recover] stale loading -> paused, src=%s elapsed=%lu tick_missing=%d",
                 source ? source : "unknown",
                 (unsigned long)pdTICKS_TO_MS(elapsed),
                 loading_ticket_missing ? 1 : 0);
        set_state(SD_PLAYER_STATE_PAUSED);
        return true;
    }

    if (pipe_state == AEL_STATE_INIT || pipe_state == AEL_STATE_STOPPED) {
        ESP_LOGW(TAG,
                 "[state_recover] stale loading -> stopped, src=%s elapsed=%lu tick_missing=%d",
                 source ? source : "unknown",
                 (unsigned long)pdTICKS_TO_MS(elapsed),
                 loading_ticket_missing ? 1 : 0);
        set_state(SD_PLAYER_STATE_STOPPED);
        return true;
    }

    ESP_LOGW(TAG,
             "[state_recover] stale loading keep busy, src=%s pipe=%d fatfs=%d dec=%d rsp=%d elapsed=%lu tick_missing=%d",
             source ? source : "unknown",
             pipe_state,
             fatfs_state,
             decoder_state,
             filter_state,
             (unsigned long)pdTICKS_TO_MS(elapsed),
             loading_ticket_missing ? 1 : 0);
    return false;
}

/*
 * DEBUG: _on_pause - internal implementation
 * Pauses playback if currently running.
 * Called from sdcard_cmd_task when SDCARD_CMD_PAUSE is received.
 */
static void _on_pause()
{
    if (audio_pipeline_get_state(s_sdcard_handler->pipeline) == AEL_STATE_RUNNING)
    {
        ESP_LOGI(TAG, "[_on_pause] Pause current audio");
        _pause_pipeline_fast();
        set_state(SD_PLAYER_STATE_PAUSED);
    }
}

/*
 * 快速暂停策略（最小改动）：
 * 按消费者 -> 生产者的逆向顺序暂停，减少大 ringbuffer 下等待排空带来的停顿。
 */
static void _pause_pipeline_fast(void)
{
    s_sdcard_handler->pause_transition_active = true;
    s_sdcard_handler->resume_guard_active = false;
    s_sdcard_handler->resume_guard_enter_tick = 0;
    s_sdcard_handler->startup_guard_active = false;
    s_sdcard_handler->startup_stable_hits = 0;
    _reset_low_water_guard_state(true, true);
    app_player_set_base_stream_read_freeze(true);
    app_player_set_base_stream_silence_wait(true);

#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_PAUSE
    _dump_pipeline_cache_status("pause_before");
#endif

    /*
     * 关键优化：
     * - 暂停只针对下游消费端（rsp_filter），优先快速静音输出，避免上游 pause 阻塞。
     * - element_raw 处于 bypass 模式（不运行任务），无需 pause。
     * - decoder/fatfs 在大 ringbuffer 满载时 pause 可能阻塞数秒（日志已验证），
     *   会直接造成“按键后很久才暂停”的体感问题。
     */
    // esp_err_t ret_raw = audio_element_pause(s_sdcard_handler->element_raw); // passive container, no need to pause
    esp_err_t ret_filter = audio_element_pause(s_sdcard_handler->element_rsp_filter);
    if (ret_filter != ESP_OK)
    {
        ESP_LOGW(TAG, "[_pause_pipeline_fast] element_pause ret filter=%d",
                 ret_filter);
    }

    // 标记 pipeline 为 PAUSED，保持控制层状态一致
    audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_PAUSED);

    /*
     * 清掉暴露给 mixer 的尾部 PCM，避免大 buffer 下暂停后仍持续播出数秒。
     * 这里只处理 raw 输入侧共享 ringbuffer，不改播放期的大 buffer 配置，
     * 以尽量降低对 WAV 启动稳定性和连续播放抗抖动能力的影响。
     */
    _flush_pause_tail_pcm();

    s_sdcard_handler->pause_transition_active = false;
    app_player_set_base_stream_read_freeze(false);
    app_player_set_base_stream_silence_wait(false);

#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_PAUSE
    _dump_pipeline_cache_status("pause_after");
#endif
}

static void _flush_pause_tail_pcm(void)
{
    ringbuf_handle_t raw_in = audio_element_get_input_ringbuf(s_sdcard_handler->element_raw);
    int filled_before = 0;

    if (raw_in != NULL)
    {
        filled_before = rb_bytes_filled(raw_in);
    }

    /*
     * raw 输入 ringbuffer 同时也是 rsp_filter 的输出 ringbuffer。
     * 暂停时复位这里，可以只裁掉已经解码好的尾部 PCM，
     * 上游 fatfs/decoder 的大 buffer 仍然保留。
     */
    audio_element_reset_input_ringbuf(s_sdcard_handler->element_raw);

    if (raw_in != NULL)
    {
        ESP_LOGI(TAG, "[_flush_pause_tail_pcm] dropped raw tail pcm: before=%d after=%d",
                 filled_before,
                 rb_bytes_filled(raw_in));
    }
}

static void _abort_pipeline_ringbufs(void)
{
    audio_element_abort_output_ringbuf(s_sdcard_handler->element_fatfs);
    audio_element_abort_input_ringbuf(s_sdcard_handler->element_decoder);
    audio_element_abort_output_ringbuf(s_sdcard_handler->element_decoder);
    audio_element_abort_input_ringbuf(s_sdcard_handler->element_rsp_filter);
    audio_element_abort_output_ringbuf(s_sdcard_handler->element_rsp_filter);
    audio_element_abort_input_ringbuf(s_sdcard_handler->element_raw);
}

/*
 * 精准 reset 当前链路 ringbuf，避免仅依赖 pipeline 级 reset。
 * 目标是清除 abort/残留数据，同时确保 raw 输出口（供 downmix 读取）被正确复位。
 */
static void _reset_pipeline_ringbufs_precise(void)
{
    audio_element_reset_output_ringbuf(s_sdcard_handler->element_fatfs);

    audio_element_reset_input_ringbuf(s_sdcard_handler->element_decoder);
    audio_element_reset_output_ringbuf(s_sdcard_handler->element_decoder);

    audio_element_reset_input_ringbuf(s_sdcard_handler->element_rsp_filter);
    audio_element_reset_output_ringbuf(s_sdcard_handler->element_rsp_filter);

    audio_element_reset_input_ringbuf(s_sdcard_handler->element_raw);
}

static void _recycle_pipeline_for_track_switch(void)
{
#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_TRACK_SWITCH
    _dump_pipeline_cache_status("switch_before_abort");
#endif

    /*
     * 自动切歌和跨格式 recycle 之前，先把已经暴露给 mixer 的尾部 PCM 裁掉。
     * 仅 reset 上游 ringbuf 不足以回收这部分旧数据，这是重叠声的关键来源。
     */
    _flush_pause_tail_pcm();
    _abort_pipeline_ringbufs();
    audio_pipeline_stop(s_sdcard_handler->pipeline);
    audio_pipeline_wait_for_stop(s_sdcard_handler->pipeline);
    audio_pipeline_terminate(s_sdcard_handler->pipeline);
    audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_INIT);
    _reset_pipeline_ringbufs_precise();
    audio_pipeline_reset_elements(s_sdcard_handler->pipeline);

#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_TRACK_SWITCH
    _dump_pipeline_cache_status("switch_after_reset");
#endif
}

static void _hot_reset_pipeline_for_track_switch(void)
{
#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_TRACK_SWITCH
    _dump_pipeline_cache_status("hot_switch_before_abort");
#endif

    /*
     * 同类型热切不销毁 pipeline task，但在 stop 前先裁掉已经暴露给 mixer
     * 的尾部 PCM，尽量避免旧歌尾音继续播出。
     * 静音/冻结由 _prepare_manual_soft_switch() 在切歌入口提前完成。
     */
    _flush_pause_tail_pcm();
    _abort_pipeline_ringbufs();
    audio_pipeline_stop(s_sdcard_handler->pipeline);
    audio_pipeline_wait_for_stop(s_sdcard_handler->pipeline);
    audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_INIT);
    _reset_pipeline_ringbufs_precise();
    audio_pipeline_reset_elements(s_sdcard_handler->pipeline);

#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_TRACK_SWITCH
    _dump_pipeline_cache_status("hot_switch_after_reset");
#endif
}

/*
 * 打印当前 pipeline 各级 ringbuffer 占用情况。
 * 重点用于暂停/切歌瞬间，确认是否存在异常积压或残留。
 */
static void _dump_pipeline_cache_status(const char *stage)
{
#if SDCARD_RB_DIAG_ENABLE
    if (s_sdcard_handler == NULL || s_sdcard_handler->pipeline == NULL)
    {
        ESP_LOGW(TAG, "[rb_diag][%s] handler/pipeline is null", stage ? stage : "null");
        return;
    }

    ringbuf_handle_t fatfs_out = audio_element_get_output_ringbuf(s_sdcard_handler->element_fatfs);
    ringbuf_handle_t dec_in = audio_element_get_input_ringbuf(s_sdcard_handler->element_decoder);
    ringbuf_handle_t dec_out = audio_element_get_output_ringbuf(s_sdcard_handler->element_decoder);
    ringbuf_handle_t rsp_in = audio_element_get_input_ringbuf(s_sdcard_handler->element_rsp_filter);
    ringbuf_handle_t rsp_out = audio_element_get_output_ringbuf(s_sdcard_handler->element_rsp_filter);
    ringbuf_handle_t raw_in = audio_element_get_input_ringbuf(s_sdcard_handler->element_raw);

    ESP_LOGI(TAG,
             "[rb_diag][%s] state pipe=%d fatfs=%d dec=%d rsp=%d raw(bypass)=%d",
             stage ? stage : "null",
             audio_pipeline_get_state(s_sdcard_handler->pipeline),
             audio_element_get_state(s_sdcard_handler->element_fatfs),
             audio_element_get_state(s_sdcard_handler->element_decoder),
             audio_element_get_state(s_sdcard_handler->element_rsp_filter),
             audio_element_get_state(s_sdcard_handler->element_raw));

    if (fatfs_out) {
        ESP_LOGI(TAG, "[rb_diag][%s] fatfs_out rb=%p filled=%d size=%d avail=%d",
                 stage ? stage : "null", fatfs_out, rb_bytes_filled(fatfs_out), rb_get_size(fatfs_out), rb_bytes_available(fatfs_out));
        _rb_diag_warn_if_needed(stage, "fatfs_out", fatfs_out);
    } else {
        ESP_LOGI(TAG, "[rb_diag][%s] fatfs_out rb=NULL", stage ? stage : "null");
    }
    if (dec_in) {
        ESP_LOGI(TAG, "[rb_diag][%s] dec_in    rb=%p filled=%d size=%d avail=%d",
                 stage ? stage : "null", dec_in, rb_bytes_filled(dec_in), rb_get_size(dec_in), rb_bytes_available(dec_in));
        _rb_diag_warn_if_needed(stage, "dec_in", dec_in);
    } else {
        ESP_LOGI(TAG, "[rb_diag][%s] dec_in    rb=NULL", stage ? stage : "null");
    }
    if (dec_out) {
        ESP_LOGI(TAG, "[rb_diag][%s] dec_out   rb=%p filled=%d size=%d avail=%d",
                 stage ? stage : "null", dec_out, rb_bytes_filled(dec_out), rb_get_size(dec_out), rb_bytes_available(dec_out));
        _rb_diag_warn_if_needed(stage, "dec_out", dec_out);
    } else {
        ESP_LOGI(TAG, "[rb_diag][%s] dec_out   rb=NULL", stage ? stage : "null");
    }
    if (rsp_in) {
        ESP_LOGI(TAG, "[rb_diag][%s] rsp_in    rb=%p filled=%d size=%d avail=%d",
                 stage ? stage : "null", rsp_in, rb_bytes_filled(rsp_in), rb_get_size(rsp_in), rb_bytes_available(rsp_in));
        _rb_diag_warn_if_needed(stage, "rsp_in", rsp_in);
    } else {
        ESP_LOGI(TAG, "[rb_diag][%s] rsp_in    rb=NULL", stage ? stage : "null");
    }
    if (rsp_out) {
        ESP_LOGI(TAG, "[rb_diag][%s] rsp_out   rb=%p filled=%d size=%d avail=%d",
                 stage ? stage : "null", rsp_out, rb_bytes_filled(rsp_out), rb_get_size(rsp_out), rb_bytes_available(rsp_out));
        _rb_diag_warn_if_needed(stage, "rsp_out", rsp_out);
    } else {
        ESP_LOGI(TAG, "[rb_diag][%s] rsp_out   rb=NULL", stage ? stage : "null");
    }
    if (raw_in) {
        ESP_LOGI(TAG, "[rb_diag][%s] raw_in    rb=%p filled=%d size=%d avail=%d",
                 stage ? stage : "null", raw_in, rb_bytes_filled(raw_in), rb_get_size(raw_in), rb_bytes_available(raw_in));
        _rb_diag_warn_if_needed(stage, "raw_in", raw_in);
    } else {
        ESP_LOGI(TAG, "[rb_diag][%s] raw_in    rb=NULL", stage ? stage : "null");
    }
#else
    (void)stage;
#endif
}

/*
 * 当 ringbuffer 占用超过设定阈值时输出告警，便于快速定位缓存堆积点。
 */
static void _rb_diag_warn_if_needed(const char *stage, const char *rb_name, ringbuf_handle_t rb)
{
#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_WARN_ENABLE
    if (rb == NULL) {
        return;
    }
    int size = rb_get_size(rb);
    int filled = rb_bytes_filled(rb);
    if (size <= 0 || filled < 0) {
        return;
    }
    int used_percent = (filled * 100) / size;
    if (used_percent >= SDCARD_RB_DIAG_WARN_THRESHOLD_PERCENT) {
        ESP_LOGW(TAG, "[rb_diag_warn][%s] %s usage=%d%% (filled=%d size=%d), threshold=%d%%",
                 stage ? stage : "null",
                 rb_name ? rb_name : "unknown",
                 used_percent, filled, size, SDCARD_RB_DIAG_WARN_THRESHOLD_PERCENT);
    }
#else
    (void)stage;
    (void)rb_name;
    (void)rb;
#endif
}

/*
 * 记录从进入 loading/低水位等待到缓存恢复的耗时。
 * 日志统一带 '#' 关键字，便于与常规业务日志分离抓取。
 */
static void _log_sd_read_cost(const char *stage, TickType_t start_tick, int filled, int rb_size, int safe_level)
{
#if SDCARD_SD_READ_COST_LOG_ENABLE
    TickType_t now_tick = 0;
    uint32_t elapsed_ms = 0;
    int used_percent = 0;
    const char *log_stage = stage ? stage : "unknown";
    const char *url = "none";

    if (s_sdcard_handler == NULL || start_tick == 0) {
        return;
    }

    now_tick = xTaskGetTickCount();
    if (now_tick < start_tick) {
        return;
    }

    elapsed_ms = (uint32_t)pdTICKS_TO_MS(now_tick - start_tick);
    if (rb_size > 0 && filled >= 0) {
        used_percent = (filled * 100) / rb_size;
    }
    if (s_sdcard_handler->current_playback_url[0] != '\0') {
        url = s_sdcard_handler->current_playback_url;
    } else if (s_sdcard_handler->current_playlist_url[0] != '\0') {
        url = s_sdcard_handler->current_playlist_url;
    }

    if (elapsed_ms >= SDCARD_SD_READ_COST_WARN_MS) {
        ESP_LOGW(TAG,
                 "[#sd_read_cost][%s] wait=%lums filled=%d/%d(%d%%) safe=%d state=%d url=%s",
                 log_stage,
                 (unsigned long)elapsed_ms,
                 filled,
                 rb_size,
                 used_percent,
                 safe_level,
                 (int)s_sdcard_handler->state,
                 url);
    } else {
        ESP_LOGD(TAG,
                 "[#sd_read_cost][%s] wait=%lums filled=%d/%d(%d%%) safe=%d state=%d url=%s",
                 log_stage,
                 (unsigned long)elapsed_ms,
                 filled,
                 rb_size,
                 used_percent,
                 safe_level,
                 (int)s_sdcard_handler->state,
                 url);
    }
#else
    (void)stage;
    (void)start_tick;
    (void)filled;
    (void)rb_size;
    (void)safe_level;
#endif
}

/*
 * 输出最终播放 ringbuf 的水位摘要，辅助判断是入口供数慢还是下游消耗过快。
 */
static void _log_output_rb_watermark(const char *stage, int filled, int rb_size, int min_level, int safe_level)
{
#if SDCARD_RB_DIAG_ENABLE
    int used_percent = 0;
    int min_percent = 0;
    int safe_percent = 0;
    const char *log_stage = stage ? stage : "unknown";

    if (rb_size <= 0) {
        return;
    }

    used_percent = (filled * 100) / rb_size;
    min_percent = (min_level > 0) ? ((min_level * 100) / rb_size) : 0;
    safe_percent = (safe_level > 0) ? ((safe_level * 100) / rb_size) : 0;

    if (filled < min_level) {
        ESP_LOGW(TAG,
                 "[#rb_watermark][%s] filled=%d/%d(%d%%) min=%d(%d%%) safe=%d(%d%%)",
                 log_stage,
                 filled,
                 rb_size,
                 used_percent,
                 min_level,
                 min_percent,
                 safe_level,
                 safe_percent);
    } else {
        ESP_LOGI(TAG,
                 "[#rb_watermark][%s] filled=%d/%d(%d%%) min=%d(%d%%) safe=%d(%d%%)",
                 log_stage,
                 filled,
                 rb_size,
                 used_percent,
                 min_level,
                 min_percent,
                 safe_level,
                 safe_percent);
    }
#else
    (void)stage;
    (void)filled;
    (void)rb_size;
    (void)min_level;
    (void)safe_level;
#endif
}

static void _handle_low_water_guard(void)
{
    ringbuf_handle_t output_rb = NULL;
    int filled = 0;
    int rb_size = 0;
    int min_level = SDCARD_LOW_WATER_MIN_LEVEL;
    int safe_level = SDCARD_LOW_WATER_SAFE_LEVEL;
    int user_volume = 0;
    uint8_t enter_hits_target = SDCARD_LOW_WATER_ENTER_HITS;
    uint8_t recover_hits_target = SDCARD_LOW_WATER_RECOVER_HITS;
    bool keep_silence_wait = false;
    bool keep_read_freeze = false;
    TickType_t now_tick = 0;
    const char *track_type = "other";

    if (s_sdcard_handler == NULL) {
        return;
    }

    now_tick = xTaskGetTickCount();

    /*
     * 启动保护/软切换期间，静音等待由 startup_guard 统一控制。
     * 这里不能再反向关闭 silence wait，否则会出现日志中
     * enable=1 后很快又被 enable=0 覆盖，导致 guard 名存实亡。
     */
    keep_silence_wait = s_sdcard_handler->pause_transition_active ||
                        s_sdcard_handler->startup_guard_active ||
                        s_sdcard_handler->soft_switch_mute_active ||
                        s_sdcard_handler->auto_next_pending;
    keep_read_freeze = keep_silence_wait;

    /*
     * 只有真正处于 MP3/WAV 播放中时才启用低水位守护。
     * 非播放态、格式不支持、尚未建立播放起点时，直接清掉本轮临时保护状态。
     */
    if (!(s_sdcard_handler->current_track_format == TRACK_FORMAT_WAV ||
          s_sdcard_handler->current_track_format == TRACK_FORMAT_MP3) ||
        s_sdcard_handler->state != SD_PLAYER_STATE_PLAYING ||
        s_sdcard_handler->current_track_play_tick == 0) {
        _reset_low_water_guard_state(keep_read_freeze, keep_silence_wait);
        return;
    }

    track_type = (s_sdcard_handler->current_track_format == TRACK_FORMAT_MP3) ? "mp3" : "wav";

    /* 起播 1 秒内不做运行期低水位判断，避免与启动预缓存阶段相互干扰。 */
    if ((now_tick - s_sdcard_handler->current_track_play_tick) < pdMS_TO_TICKS(1000)) {
        return;
    }

    if (audio_pipeline_get_state(s_sdcard_handler->pipeline) != AEL_STATE_RUNNING ||
        audio_element_get_state(s_sdcard_handler->element_fatfs) != AEL_STATE_RUNNING ||
        audio_element_get_state(s_sdcard_handler->element_decoder) != AEL_STATE_RUNNING ||
        audio_element_get_state(s_sdcard_handler->element_rsp_filter) != AEL_STATE_RUNNING) {
        _reset_low_water_guard_state(keep_read_freeze, keep_silence_wait);
        return;
    }

    output_rb = s_sdcard_handler->output_rb;
    if (output_rb == NULL) {
        return;
    }

    filled = rb_bytes_filled(output_rb);
    rb_size = rb_get_size(output_rb);
    if (rb_size <= 0) {
        return;
    }

    /* 依据当前格式和 RB 容量，动态算出进入/恢复门限与命中次数。 */
    _calc_track_low_water_levels(s_sdcard_handler->current_track_format,
                                 rb_size,
                                 &min_level,
                                 &safe_level,
                                 &enter_hits_target,
                                 &recover_hits_target);

    if (app_player_get_user_volume(&user_volume) != ESP_OK) {
        user_volume = s_sdcard_handler->low_water_saved_volume;
    }
    if (user_volume < 0) {
        user_volume = 0;
    }
    if (user_volume > 100) {
        user_volume = 100;
    }

    /*
     * 第一阶段：检测是否需要“进入低水位保护”。
     * 这里使用连续命中计数，而不是单次跌破就触发，避免被短时调度抖动误伤。
     */
    if (!s_sdcard_handler->low_water_guard_active) {
        s_sdcard_handler->low_water_recover_hits = 0;

        if (filled < min_level) {
            if (s_sdcard_handler->low_water_enter_hits < 0xFFU) {
                s_sdcard_handler->low_water_enter_hits++;
            }
        } else {
            s_sdcard_handler->low_water_enter_hits = 0;
        }

        if (s_sdcard_handler->low_water_enter_hits >= enter_hits_target) {
            /*
             * 进入低水位保护时，先从当前用户音量开始渐弱，而不是立刻硬切静音。
             * 这样主观听感更像“播放器主动缓冲”，不会产生突兀断崖感。
             */
            s_sdcard_handler->low_water_guard_active = true;
            s_sdcard_handler->low_water_enter_hits = 0;
            s_sdcard_handler->low_water_duck_state = LOW_WATER_DUCK_STATE_FADING_OUT;
            s_sdcard_handler->low_water_enter_tick = now_tick;
            s_sdcard_handler->low_water_saved_volume = user_volume;
            s_sdcard_handler->low_water_duck_volume = user_volume;
            app_player_set_base_stream_low_water_duck(true, user_volume);
            _log_output_rb_watermark("low_enter", filled, rb_size, min_level, safe_level);
#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_LOW_WATER
            _dump_pipeline_cache_status("low_enter");
#endif
            ESP_LOGW(TAG,
                     "[low_water] type=%s duck enter, filled=%d min=%d safe=%d rb=%d user=%d hits=%u",
                     track_type,
                     filled,
                     min_level,
                     safe_level,
                     rb_size,
                     user_volume,
                     (unsigned int)enter_hits_target);
        }
    }

    if (!s_sdcard_handler->low_water_guard_active) {
        return;
    }

    /*
     * 第二阶段：已进入低水位保护，但长时间仍无法恢复。
     * 此时说明当前曲目供数能力持续不足，再等下去意义不大，直接切下一首兜底。
     */
    if ((now_tick - s_sdcard_handler->low_water_enter_tick) >= pdMS_TO_TICKS(SDCARD_LOW_WATER_TIMEOUT_MS)) {
        app_player_set_base_stream_low_water_duck(true, 0);
        app_player_set_base_stream_read_freeze(true);
        app_player_set_base_stream_silence_wait(true);
        s_sdcard_handler->low_water_duck_state = LOW_WATER_DUCK_STATE_WAIT_RECOVER;
        _log_sd_read_cost("low_timeout",
                          s_sdcard_handler->low_water_enter_tick,
                          filled,
                          rb_size,
                          safe_level);
        _log_output_rb_watermark("low_timeout", filled, rb_size, min_level, safe_level);
#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_LOW_WATER
        _dump_pipeline_cache_status("low_timeout");
#endif
        ESP_LOGW(TAG,
                 "[low_water] type=%s timeout next force, filled=%d safe=%d saved=%d",
                 track_type,
                 filled,
                 safe_level,
                 s_sdcard_handler->low_water_saved_volume);
        if (on_next_song_force() != ESP_OK) {
            ESP_LOGW(TAG, "[low_water] force next enqueue failed, keep wait state");
            return;
        }
        s_sdcard_handler->soft_switch_mute_active = true;
        _reset_low_water_guard_state(true, true);
        return;
    }

    /*
     * 第三阶段：统计恢复命中次数。
     * 只有缓存连续回到安全区，才允许从等待态切回正常播放，避免反复拉扯。
     */
    if (filled >= safe_level) {
        if (s_sdcard_handler->low_water_recover_hits < 0xFFU) {
            s_sdcard_handler->low_water_recover_hits++;
        }
    } else {
        s_sdcard_handler->low_water_recover_hits = 0;
    }

    if (s_sdcard_handler->low_water_recover_hits >= recover_hits_target) {
        s_sdcard_handler->low_water_recover_hits = 0;
        if (s_sdcard_handler->low_water_duck_state == LOW_WATER_DUCK_STATE_WAIT_RECOVER) {
            app_player_set_base_stream_read_freeze(false);
            app_player_set_base_stream_silence_wait(false);
        }
        s_sdcard_handler->low_water_duck_state = LOW_WATER_DUCK_STATE_FADING_IN;
        _log_sd_read_cost("low_recover",
                          s_sdcard_handler->low_water_enter_tick,
                          filled,
                          rb_size,
                          safe_level);
        _log_output_rb_watermark("low_recover", filled, rb_size, min_level, safe_level);
#if SDCARD_RB_DIAG_ENABLE && SDCARD_RB_DIAG_ON_LOW_WATER
        _dump_pipeline_cache_status("low_recover");
#endif
        ESP_LOGI(TAG,
                 "[low_water] type=%s recover begin fade in, filled=%d safe=%d current=%d hits=%u",
                 track_type,
                 filled,
                 safe_level,
                 s_sdcard_handler->low_water_duck_volume,
                 (unsigned int)recover_hits_target);
    }

    switch (s_sdcard_handler->low_water_duck_state)
    {
    case LOW_WATER_DUCK_STATE_FADING_OUT:
        /* 渐弱阶段：每轮下调一点音量，直到完全静音，再冻结真实读取。 */
        s_sdcard_handler->low_water_duck_volume -= SDCARD_LOW_WATER_DUCK_STEP;
        if (s_sdcard_handler->low_water_duck_volume < 0) {
            s_sdcard_handler->low_water_duck_volume = 0;
        }
        app_player_set_base_stream_low_water_duck(true, s_sdcard_handler->low_water_duck_volume);
        if (s_sdcard_handler->low_water_duck_volume == 0) {
            app_player_set_base_stream_read_freeze(true);
            app_player_set_base_stream_silence_wait(true);
            s_sdcard_handler->low_water_duck_state = LOW_WATER_DUCK_STATE_WAIT_RECOVER;
            ESP_LOGI(TAG, "[low_water] type=%s fade out done, pause wait recover", track_type);
        }
        break;

    case LOW_WATER_DUCK_STATE_WAIT_RECOVER:
        /* 等待恢复阶段：保持静音与冻结，只允许上游继续补仓。 */
        app_player_set_base_stream_low_water_duck(true, 0);
        break;

    case LOW_WATER_DUCK_STATE_FADING_IN:
        /* 渐强阶段：缓存恢复稳定后，平滑把音量拉回用户当前设置值。 */
        if (s_sdcard_handler->low_water_duck_volume < user_volume) {
            s_sdcard_handler->low_water_duck_volume += SDCARD_LOW_WATER_DUCK_STEP;
            if (s_sdcard_handler->low_water_duck_volume > user_volume) {
                s_sdcard_handler->low_water_duck_volume = user_volume;
            }
            app_player_set_base_stream_low_water_duck(true, s_sdcard_handler->low_water_duck_volume);
        }
        if (s_sdcard_handler->low_water_duck_volume >= user_volume) {
            _reset_low_water_guard_state(keep_read_freeze, keep_silence_wait);
            ESP_LOGI(TAG, "[low_water] type=%s fade in done, user=%d", track_type, user_volume);
        }
        break;

    case LOW_WATER_DUCK_STATE_IDLE:
    default:
        break;
    }
}

static void sdcard_low_water_guard_task(void *pv)
{
    (void)pv;

    while (1)
    {
        /* 常驻后台轮询，统一处理 MP3/WAV 的运行期低水位保护。 */
        _handle_low_water_guard();
        vTaskDelay(pdMS_TO_TICKS(SDCARD_LOW_WATER_CHECK_MS));
    }
}

/*
 * DEBUG: _on_stop - internal implementation
 * Stops playback completely.
 * Called from sdcard_cmd_task when SDCARD_CMD_STOP is received.
 */
static void _on_stop()
{
    s_sdcard_handler->resume_guard_active = false;
    s_sdcard_handler->resume_guard_enter_tick = 0;
    _reset_low_water_guard_state(false, false);
    s_sdcard_handler->switch_debounce_active = false;
    s_sdcard_handler->pending_switch_steps = 0;
    _abort_pipeline_ringbufs();
    audio_pipeline_stop(s_sdcard_handler->pipeline);
    audio_pipeline_wait_for_stop(s_sdcard_handler->pipeline);
    audio_pipeline_terminate(s_sdcard_handler->pipeline);
    audio_pipeline_change_state(s_sdcard_handler->pipeline, AEL_STATE_INIT);
    _reset_pipeline_ringbufs_precise();
    audio_pipeline_reset_elements(s_sdcard_handler->pipeline);
    set_state(SD_PLAYER_STATE_STOPPED);
}

/*
 * ============================================================================
 * DEBUG: Event Queue Command Processing
 * ============================================================================
 */

/*
 * DEBUG: send_cmd - Helper function to post a command to the queue
 * This is called by the public wrapper functions to enqueue commands.
 * Returns ESP_OK on success, ESP_FAIL if queue is not initialized or full.
 */
static esp_err_t send_cmd(sdcard_cmd_t cmd)
{
    UBaseType_t waiting = 0;

    if (s_sdcard_handler == NULL || s_sdcard_handler->cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "[send_cmd] Queue not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // DEBUG: Log the command being sent
    ESP_LOGI(TAG, "[send_cmd] Posting command: %s", cmd_log_str[cmd]);

    waiting = uxQueueMessagesWaiting(s_sdcard_handler->cmd_queue);
    if (waiting >= SDCARD_CMD_QUEUE_RECOVER_THRESHOLD &&
        (s_sdcard_handler->state == SD_PLAYER_STATE_LOADING ||
         s_sdcard_handler->state == SD_PLAYER_STATE_SWITCHING))
    {
        s_sdcard_handler->queue_recover_pending = true;
        xQueueReset(s_sdcard_handler->cmd_queue);
        ESP_LOGW(TAG, "[send_cmd] queue pressure recover waiting=%u cmd=%s",
                 (unsigned int)waiting,
                 cmd_log_str[cmd]);
    }

    // Post to queue with no wait (non-blocking)
    if (xQueueSend(s_sdcard_handler->cmd_queue, &cmd, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "[send_cmd] Queue full, command %s dropped", cmd_log_str[cmd]);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/*
 * DEBUG: sdcard_cmd_task - Command processing task
 * This task waits for commands on the queue and dispatches them to the
 * appropriate internal implementation functions.
 * 
 * Task priority: 10 (adjustable)
 * Stack size: 4KB
 */
static void sdcard_cmd_task(void *pv)
{
    sdcard_cmd_t cmd;

    ESP_LOGI(TAG, "[sdcard_cmd_task] Task started, waiting for commands...");

    while (1)
    {
        // Block indefinitely waiting for a command
        if (xQueueReceive(s_sdcard_handler->cmd_queue, &cmd, portMAX_DELAY) == pdTRUE)
        {
            // DEBUG: Log received command
            ESP_LOGI(TAG, "[sdcard_cmd_task] Received command: %s", cmd_log_str[cmd]);

            if (s_sdcard_handler->queue_recover_pending)
            {
                s_sdcard_handler->queue_recover_pending = false;
                s_sdcard_handler->switch_debounce_active = false;
                s_sdcard_handler->pending_switch_steps = 0;
                _recycle_pipeline_for_track_switch();
                _reset_low_water_guard_state(false, false);
                set_state(SD_PLAYER_STATE_STOPPED);
                ESP_LOGW(TAG, "[sdcard_cmd_task] queue pressure recovery done");
            }

            // Dispatch to the appropriate internal function
            switch (cmd)
            {
            case SDCARD_CMD_NEXT:
                _on_next_song();
                break;
            case SDCARD_CMD_PREV:
                _on_prev_song();
                break;
            case SDCARD_CMD_NEXT_FORCE:
                _on_next_song_force();
                break;
            case SDCARD_CMD_SWITCH_COMMIT:
                _commit_pending_switch();
                break;
            case SDCARD_CMD_PLAY_PAUSE:
                _on_play_pause();
                break;
            case SDCARD_CMD_STOP:
                _on_stop();
                break;
            case SDCARD_CMD_PAUSE:
                _on_pause();
                break;
            case SDCARD_CMD_STATE_CHECK:
                _play_state_check_impl();
                break;
            default:
                ESP_LOGW(TAG, "[sdcard_cmd_task] Unknown command: %d", cmd);
                break;
            }

            // DEBUG: Log command completion
            ESP_LOGD(TAG, "[sdcard_cmd_task] Command %s completed", cmd_log_str[cmd]);
        }
    }
}

/*
 * ============================================================================
 * DEBUG: Public Wrapper Functions
 * These functions are called by external code and post commands to the queue.
 * The actual work is done by sdcard_cmd_task calling the internal functions.
 * ============================================================================
 */

// DEBUG: on_next_song wrapper - posts SDCARD_CMD_NEXT to queue
static void on_next_song()
{
    if (s_sdcard_handler == NULL) {
        return;
    }
    send_cmd(SDCARD_CMD_NEXT);
}

static esp_err_t on_next_song_force(void)
{
    if (s_sdcard_handler == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return send_cmd(SDCARD_CMD_NEXT_FORCE);
}

// DEBUG: on_prev_song wrapper - posts SDCARD_CMD_PREV to queue
static void on_prev_song()
{
    if (s_sdcard_handler == NULL) {
        return;
    }
    send_cmd(SDCARD_CMD_PREV);
}

// DEBUG: on_play_pause wrapper - posts SDCARD_CMD_PLAY_PAUSE to queue
static void on_play_pause()
{
    send_cmd(SDCARD_CMD_PLAY_PAUSE);
}

// DEBUG: on_stop wrapper - posts SDCARD_CMD_STOP to queue
void on_stop()
{
    send_cmd(SDCARD_CMD_STOP);
}

// DEBUG: on_pause wrapper - posts SDCARD_CMD_PAUSE to queue
static void on_pause()
{
    send_cmd(SDCARD_CMD_PAUSE);
}

esp_err_t on_init(audio_event_iface_handle_t evt)
{
    int sdcard_rb_size = get_sdcard_rb_size();

    if (s_sdcard_handler == NULL)
    {
        s_sdcard_handler = (sdcard_audio_handler_t *)audio_calloc(1, sizeof(sdcard_audio_handler_t));
        memset(s_sdcard_handler, 0, sizeof(sdcard_audio_handler_t));
    }

    ESP_LOGI(TAG, "[sdcard_player][on_init] Create Fatfs stream to read input data");
    fatfs_stream_cfg_t fatfs_cfg = FATFS_STREAM_CFG_DEFAULT();
    fatfs_cfg.out_rb_size = 256 * 1024; // 增大 FatFs 输出缓冲至 256KB，防止 SD 卡长耗时阻塞导致解码器饥饿
    fatfs_cfg.ext_stack = true;
    fatfs_cfg.type = AUDIO_STREAM_READER;
    s_sdcard_handler->element_fatfs = fatfs_stream_init(&fatfs_cfg);
    if (s_sdcard_handler->element_fatfs == NULL)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] fatfs init fail");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[sdcard_player][on_init] Create mp3 decoder to decode mp3 file");
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
    auto_dec_cfg.task_prio = SDCARD_DECODER_TASK_PRIO_DEFAULT;
    auto_dec_cfg.out_rb_size = sdcard_rb_size;
    s_sdcard_handler->element_decoder = esp_decoder_init(&auto_dec_cfg, auto_decode, sizeof(auto_decode) / sizeof(audio_decoder_t));
    if (s_sdcard_handler->element_decoder == NULL)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] element_decoder init fail");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[sdcard_player][on_init] Create resample element");
    rsp_filter_cfg_t rsp_sdcard_cfg = DEFAULT_RESAMPLE_FILTER_CONFIG();
    rsp_sdcard_cfg.src_rate = 44100;
    rsp_sdcard_cfg.src_ch = 2;
    rsp_sdcard_cfg.dest_rate = OUTPUT_SAMPLERATE;
    rsp_sdcard_cfg.dest_ch = 1;
    rsp_sdcard_cfg.out_rb_size = sdcard_rb_size;
    s_sdcard_handler->element_rsp_filter = rsp_filter_init(&rsp_sdcard_cfg);
    if (s_sdcard_handler->element_rsp_filter == NULL)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] element_rsp_filter init fail");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "[sdcard_player][on_init] Create raw stream of base mp3 to write data");
    raw_stream_cfg_t raw_cfg = RAW_STREAM_CFG_DEFAULT();
    raw_cfg.type = AUDIO_STREAM_WRITER;
    raw_cfg.out_rb_size = sdcard_rb_size;
    s_sdcard_handler->element_raw = raw_stream_init(&raw_cfg);
    if (s_sdcard_handler->element_raw == NULL)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] element_raw init fail");
        return ESP_FAIL;
    }

    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
    s_sdcard_handler->pipeline = audio_pipeline_init(&pipeline_cfg);
    mem_assert(s_sdcard_handler->pipeline);
    audio_pipeline_register(s_sdcard_handler->pipeline, s_sdcard_handler->element_fatfs, "sdcard_fatfs");
    audio_pipeline_register(s_sdcard_handler->pipeline, s_sdcard_handler->element_decoder, "sdcard_decoder");
    audio_pipeline_register(s_sdcard_handler->pipeline, s_sdcard_handler->element_rsp_filter, "sdcard_filter");
    audio_pipeline_register(s_sdcard_handler->pipeline, s_sdcard_handler->element_raw, "sdcard_raw");
    const char *link_tag_base[4] = {"sdcard_fatfs", "sdcard_decoder", "sdcard_filter", "sdcard_raw"};
    audio_pipeline_link(s_sdcard_handler->pipeline, &link_tag_base[0], 4);
    s_sdcard_handler->output_rb = audio_element_get_input_ringbuf(s_sdcard_handler->element_raw);

    // downmix_set_input_rb(s_player->mixer.element_mixer, rb_base, 0); // 需要在主player中设置

    ESP_LOGI(TAG, "[sdcard_player][on_init] Set up  sdcard event listener");
    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
    // 调大事件队列，防止高频率切歌或解码器抛出大量状态事件时溢出
    evt_cfg.internal_queue_size = 160;
    evt_cfg.external_queue_size = 160;
    evt_cfg.queue_set_size = 160;
    audio_event_iface_handle_t sdcard_evt = audio_event_iface_init(&evt_cfg);
    audio_pipeline_set_listener(s_sdcard_handler->pipeline, sdcard_evt);
    esp_err_t ret = audio_thread_create(&s_sdcard_handler->sdcard_handler, "palyer_event_handler2", sdcard_event_handler,
                                        sdcard_evt, 3 * 1024, 13, true, 1);
    // ret = xTaskCreatePinnedToCore(app_palyer_event_handler, "palyer_event_handler", (10 * 1024), NULL, 15, NULL, ESP_TASK_MAIN_CORE);
    if (ret == ESP_FAIL)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] Create audio manager task failure");
        ret = ESP_ERR_AUDIO_MEMORY_LACK;
        // goto ep_init_err;
    }

    s_sdcard_handler->evt_out = evt;

    /*
     * DEBUG: Create command queue and task
     * The command queue is used to serialize music control commands.
     * The sdcard_cmd_task processes these commands sequentially.
     */
    ESP_LOGI(TAG, "[sdcard_player][on_init] Creating command queue...");
    s_sdcard_handler->cmd_queue = xQueueCreate(SDCARD_CMD_QUEUE_SIZE, sizeof(sdcard_cmd_t));
    if (s_sdcard_handler->cmd_queue == NULL)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] Failed to create command queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "[sdcard_player][on_init] Creating command processing task...");
    BaseType_t task_ret = xTaskCreatePinnedToCore(
        sdcard_cmd_task,           // Task function
        "sdcard_cmd_task",         // Task name
        4 * 1024,                  // Stack size (4KB)
        NULL,                      // Task parameter
        10,                        // Priority (adjustable)
        NULL,                      // Task handle (not needed)
        1                          // Core ID
    );
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] Failed to create command task");
        vQueueDelete(s_sdcard_handler->cmd_queue);
        s_sdcard_handler->cmd_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "[sdcard_player][on_init] Command queue and task created successfully");

    task_ret = xTaskCreatePinnedToCore(
        sdcard_low_water_guard_task,
        "sdcard_rb_guard",
        3 * 1024,
        NULL,
        6,
        NULL,
        1
    );
    if (task_ret != pdPASS)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] Failed to create low water guard task");
        return ESP_ERR_NO_MEM;
    }

    s_sdcard_handler->lock = mutex_create();
    s_sdcard_handler->task_prio_baseline_valid = false;
    s_sdcard_handler->fatfs_task_prio_baseline = 0;
    s_sdcard_handler->decoder_task_prio_baseline = 0;
    s_sdcard_handler->filter_task_prio_baseline = 0;
    s_sdcard_handler->pending_switch_steps = 0;
    s_sdcard_handler->switch_debounce_active = false;
    s_sdcard_handler->queue_recover_pending = false;
    s_sdcard_handler->track_total = 0U;
    s_sdcard_handler->track_index = 0U;
    s_sdcard_handler->track_scan_signature = 0U;
    s_sdcard_handler->current_track_format = TRACK_FORMAT_UNKNOWN;
    s_sdcard_handler->current_track_is_wav = false;
    s_sdcard_handler->current_track_play_tick = 0;
    s_sdcard_handler->loading_enter_tick = 0;
    s_sdcard_handler->soft_switch_mute_active = false;
    s_sdcard_handler->startup_guard_active = false;
    s_sdcard_handler->resume_guard_active = false;
    s_sdcard_handler->resume_guard_enter_tick = 0;
    s_sdcard_handler->startup_stable_hits = 0;
    s_sdcard_handler->auto_next_pending = false;
    s_sdcard_handler->auto_next_wait_tick = 0;
    s_sdcard_handler->auto_next_stable_hits = 0;
    s_sdcard_handler->low_water_guard_active = false;
    s_sdcard_handler->low_water_enter_hits = 0;
    s_sdcard_handler->low_water_recover_hits = 0;
    s_sdcard_handler->low_water_duck_state = LOW_WATER_DUCK_STATE_IDLE;
    s_sdcard_handler->low_water_enter_tick = 0;
    s_sdcard_handler->low_water_saved_volume = 0;
    s_sdcard_handler->low_water_duck_volume = 0;
    s_sdcard_handler->scan_task_handle = NULL;
    s_sdcard_handler->scan_task_running = false;
    s_sdcard_handler->scan_activate_cache = false;
    s_sdcard_handler->cache_swap_pending = false;

    // set instance
    s_sdcard_handler->interface.get_ringbuf = get_output_rb;
    s_sdcard_handler->interface.next = on_next_song;
    s_sdcard_handler->interface.prev = on_prev_song;
    // s_sdcard_handler->interface.reset = on_sdcard_scan;
    s_sdcard_handler->interface.play_pause = on_play_pause;
    s_sdcard_handler->interface.stop = on_stop;
    s_sdcard_handler->interface.pause = on_pause;
    s_sdcard_handler->interface.is_playing = is_playing;

    ret = _prepare_track_cache_on_startup();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "[sdcard_player][on_init] Failed to prepare track cache");
        return ret;
    }

    ESP_LOGI(TAG, "[sdcard_player][on_init] player_sdcard_init success");
    // if(s_player->current_base_pipeline == NULL){
    //     app_switch2_sdcard();
    //     s_player->current_base_pipeline = s_sdcard_handler->pipeline;
    //     ESP_LOGI(TAG,"Set current_base_pipeline to sdcard_audio.pipeline");
    // }
    ESP_LOGW(TAG, "[sdcard_player][on_init] sdcard pipeline:%d,fatfs:%d,decoder:%d,rsp_filter:%d,raw:%d,tracks:%lu", (int)(s_sdcard_handler->pipeline),
             (int)(s_sdcard_handler->element_fatfs), (int)(s_sdcard_handler->element_decoder),
             (int)(s_sdcard_handler->element_rsp_filter), (int)(s_sdcard_handler->element_raw),
             (unsigned long)s_sdcard_handler->track_total);

    return ESP_OK;
}

esp_err_t app_player_sdcard_init(audio_event_iface_handle_t evt)
{
    if (s_sdcard_handler == NULL || s_sdcard_handler->element_fatfs == NULL || s_sdcard_handler->pipeline == NULL)
    {
        return on_init(evt);
    }
    else
    {
        return _prepare_track_cache_on_startup();
    }
    return ESP_OK;
}

sdcard_player_t *app_player_sdcard_get_interface()
{
    if (s_sdcard_handler)
        return &s_sdcard_handler->interface;
    return NULL;
}
