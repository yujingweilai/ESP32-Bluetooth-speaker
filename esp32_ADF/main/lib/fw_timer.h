#ifndef FW_TIMER_H
#define FW_TIMER_H
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

#include "stdint.h"

// --- 可配置的宏 ---
// 定义本模块能够管理的最大定时器数量
#define FW_MAX_TIMERS           (50)
// 定义哈希表的大小，推荐为 2 的幂，以便使用位运算代替取模
#define FW_TIMER_HASH_TABLE_SIZE (16) 


// 为了代码清晰和可移植性，定义基本数据类型
typedef uint16_t U16;
typedef uint32_t U32;
typedef int8_t   S8;

/**
 * @brief 定时器回调函数的函数指针类型定义
 * @param timerId 用户在设置定时器时传入的 timerId
 * @param msg     用户在设置定时器时传入的私有数据指针
 */
typedef void (*Handler)(U16 timerId, void *msg);

/**
 * @brief 初始化定时器模块
 * @details 
 * - 在使用任何其他定时器函数之前，必须先调用此函数。
 * - 此函数会初始化内部的内存池和哈希表，并创建互斥锁。
 * @return 成功返回 pdPASS，失败返回 pdFAIL
 */
S8 FW_TimerInit(void);

/**
 * @brief 设置一个一次性定时器 (one-shot timer)
 * @details
 * - 如果一个具有相同 `handler` 和 `timerId` 的定时器已经存在，它将被重置。
 * - 定时器触发后其资源不会自动释放，需要再次调用 FW_SetTimer 重置或 FW_ReleaseTimer 释放。
 *
 * @param handler 定时器超时后要调用的回调函数
 * @param timerId 用户定义的定时器ID
 * @param msg     一个将传递给回调函数的指针，可用于传递任意用户数据
 * @param delay   定时器延迟时间，单位：毫秒 (ms)
 * @return 成功返回 pdPASS，如果超过最大定时器数量限制或其它失败则返回 pdFAIL
 */
S8 FW_SetTimer(Handler handler, U16 timerId, void *msg, U32 delay);

/**
 * @brief 释放一个之前设置的定时器
 * @details
 * - 此函数会根据 `handler` 和 `timerId` 的组合来唯一地标识并删除一个定时器。
 *
 * @param handler 要释放的定时器的回调函数
 * @param timerId   要释放的定时器的ID
 */
void FW_ReleaseTimer(Handler handler, U16 timerId);

/**
 * @brief Check if a timer exists and is active
 * @param handler Timer callback function
 * @param timerId Timer ID
 * @return pdTRUE if active, pdFALSE otherwise
 */
S8 FW_CheckTimer(Handler handler, U16 timerId);


#endif // FW_TIMER_H

