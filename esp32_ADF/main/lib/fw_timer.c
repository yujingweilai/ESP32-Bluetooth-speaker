#include "fw_timer.h"
#include <string.h> // for memset
#include "esp_log.h"

static const char* TAG = "fw_timer";

/**
 * @brief 定时器管理节点的结构体
 */
typedef struct FwTimerNode {
    TimerHandle_t       xTimer;       // FreeRTOS 定时器句柄 (缓存)
    Handler             handler;      // 用户回调
    U16                 timerId;      // 用户ID
    void                *msg;         // 用户参数
    bool                is_allocated; // 标记该节点是否正在使用
} FwTimerNode_t;

// === 模块内静态全局变量 ===
// 1. 静态数组: 直接存储所有节点，不再使用链表和哈希表
static FwTimerNode_t g_timerNodes[FW_MAX_TIMERS];

// 2. 互斥锁: 保护数组访问
static SemaphoreHandle_t g_xTimerListMutex = NULL;

// === 模块内静态函数声明 ===
static void prvTimerCallback(TimerHandle_t xTimer);


/**
 * @brief 初始化定时器模块
 */
S8 FW_TimerInit(void) {
    if (g_xTimerListMutex != NULL) {
        return pdPASS; // 防止重复初始化
    }

    g_xTimerListMutex = xSemaphoreCreateMutex();
    if (g_xTimerListMutex == NULL) {
        return pdFAIL;
    }

    // 清零所有节点状态
    memset(g_timerNodes, 0, sizeof(g_timerNodes));
    // 注意：此时 xTimer 均为 NULL，将在第一次使用时创建

    return pdPASS;
}

/**
 * @brief 设置一个一次性定时器 (极速优化版)
 * @details 
 * 1. 使用线性扫描代替哈希表 (对于 N=20，线性扫描比哈希计算+链表遍历更快且更省内存)
 * 2. 缓存 FreeRTOS Timer 句柄 (对象池模式)，避免反复 Create/Delete 的巨大开销
 */
S8 FW_SetTimer(Handler handler, U16 timerId, void *msg, U32 delay) {
    if (g_xTimerListMutex == NULL || handler == NULL) {
        return pdFAIL;
    }

    // 检查延时参数
    if (delay > (UINT32_MAX / configTICK_RATE_HZ)) {
        ESP_LOGE(TAG, "Delay %lu ms exceeds max safe value", delay);
    }

    TickType_t xTimerPeriodInTicks = pdMS_TO_TICKS(delay);
    if (xTimerPeriodInTicks == 0) {
        xTimerPeriodInTicks = 1;
    }

    if (xSemaphoreTake(g_xTimerListMutex, portMAX_DELAY) != pdTRUE) {
        return pdFAIL;
    }

    int freeIndex = -1;
    
    // 1. 扫描数组：查找匹配项 或 空闲项
    for (int i = 0; i < FW_MAX_TIMERS; i++) {
        if (g_timerNodes[i].is_allocated) {
            // 检查是否匹配
            if (g_timerNodes[i].handler == handler && g_timerNodes[i].timerId == timerId) {
                // === 找到匹配项：复用 ===
                g_timerNodes[i].msg = msg;
                
                // 重置定时器周期并启动 (如果已运行则重置倒计时，如果已停止则启动)
                // 注意：xTimerChangePeriod 会自动启动定时器
                if (xTimerChangePeriod(g_timerNodes[i].xTimer, xTimerPeriodInTicks, 0) != pdPASS) {
                    xSemaphoreGive(g_xTimerListMutex);
                    return pdFAIL; // 命令队列满
                }
                
                xSemaphoreGive(g_xTimerListMutex);
                return pdPASS;
            }
        } else {
            // 记录遇到的第一个空闲位置
            if (freeIndex == -1) {
                freeIndex = i;
            }
        }
    }

    // 2. 未找到匹配项，使用空闲位置创建新任务
    if (freeIndex != -1) {
        FwTimerNode_t *pNode = &g_timerNodes[freeIndex];
        
        // 如果该槽位之前从未创建过 Timer 句柄，则创建之
        if (pNode->xTimer == NULL) {
            // 在锁内创建是安全的，因为这是初始化操作，频率极低
            pNode->xTimer = xTimerCreate("FW_Tmr", 1, pdFALSE, (void *)pNode, prvTimerCallback);
            if (pNode->xTimer == NULL) {
                xSemaphoreGive(g_xTimerListMutex);
                return pdFAIL; // 内存不足
            }
        }

        // 填充信息
        pNode->handler = handler;
        pNode->timerId = timerId;
        pNode->msg = msg;
        pNode->is_allocated = true;

        // 启动定时器 (设置周期并启动)
        if (xTimerChangePeriod(pNode->xTimer, xTimerPeriodInTicks, 0) != pdPASS) {
            // 启动失败，回滚状态
            pNode->is_allocated = false;
            xSemaphoreGive(g_xTimerListMutex);
            return pdFAIL;
        }

        xSemaphoreGive(g_xTimerListMutex);
        return pdPASS;
    }

    // 3. 既没找到匹配项，也没找到空闲位 -> 满了
    xSemaphoreGive(g_xTimerListMutex);
    return pdFAIL;
}

/**
 * @brief 释放定时器
 * @details 仅停止定时器并标记为空闲，保留 xTimer 句柄以供下次复用
 */
void FW_ReleaseTimer(Handler handler, U16 timerId) {
    if (g_xTimerListMutex == NULL || handler == NULL) {
        return;
    }
    
    if (xSemaphoreTake(g_xTimerListMutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < FW_MAX_TIMERS; i++) {
            if (g_timerNodes[i].is_allocated && 
                g_timerNodes[i].handler == handler && 
                g_timerNodes[i].timerId == timerId) {
                
                // 停止定时器
                xTimerStop(g_timerNodes[i].xTimer, 0);
                // 标记为未分配，但保留 xTimer 句柄
                g_timerNodes[i].is_allocated = false;
                break;
            }
        }
        xSemaphoreGive(g_xTimerListMutex);
    }
}

/**
 * @brief 检查定时器是否活动
 */
S8 FW_CheckTimer(Handler handler, U16 timerId) {
    if (g_xTimerListMutex == NULL || handler == NULL) {
        return pdFALSE;
    }

    if (xSemaphoreTake(g_xTimerListMutex, portMAX_DELAY) == pdTRUE) {
        for (int i = 0; i < FW_MAX_TIMERS; i++) {
            if (g_timerNodes[i].is_allocated && 
                g_timerNodes[i].handler == handler && 
                g_timerNodes[i].timerId == timerId) {
                
                // 检查 FreeRTOS 定时器是否处于活动状态
                if (xTimerIsTimerActive(g_timerNodes[i].xTimer) != pdFALSE) {
                    xSemaphoreGive(g_xTimerListMutex);
                    return pdTRUE;
                }
                // 如果已分配但不活动（例如刚停止），继续查找（理论上唯一，可以直接返回 FALSE）
                break;
            }
        }
        xSemaphoreGive(g_xTimerListMutex);
    }
    return pdFALSE;
}

/**
 * @brief FreeRTOS 定时器回调
 */
static void prvTimerCallback(TimerHandle_t xTimer) {
    FwTimerNode_t *pNode = (FwTimerNode_t *)pvTimerGetTimerID(xTimer);
    
    // 再次检查 is_allocated，防止在回调触发前被释放的极端情况
    // (虽然 xTimerStop 会尝试从队列移除命令，但双重检查更安全)
    if (pNode && pNode->is_allocated && pNode->handler) {
        // 执行用户回调
        pNode->handler(pNode->timerId, pNode->msg);
        
        // 注意：这是一次性定时器。
        // FreeRTOS 自动停止了它。
        // 我们是否需要自动释放节点 (is_allocated = false)?
        // 原始逻辑没有自动释放，而是保留直到用户显式 Release 或 Set(Reset)。
        // 但通常一次性定时器触发后，逻辑上它就“结束”了。
        // 为了保持与原 API 行为一致（原版 SetTimer 会 Reset，Release 会 Delete），
        // 这里我们保持 is_allocated = true。
        // 这样用户下次调用 SetTimer 时可以复用同一个 slot 和 handle。
        // 如果用户希望触发后自动释放资源，需要在回调里调用 Release。
        // 但为了防止 slot 泄漏，如果用户不调用 Release 怎么办？
        // 
        // 优化策略：
        // 对于一次性定时器，触发后它实际上已经不占用 CPU 资源了（除了占用一个 slot）。
        // 鉴于我们有固定大小的池 (20)，如果用户不释放，池子会满。
        // 
        // 改进：当回调执行完毕，我们可以认为这个一次性任务完成了。
        // 如果我们把 is_allocated 设为 false，下次 SetTimer 就会把它当做新任务，
        // 但会复用 xTimer 句柄（因为 xTimer 不为 NULL）。
        // 这样既自动回收了 slot，又保留了句柄缓存。完美。
        
        // 获取锁来修改状态
        // 注意：在回调中获取互斥锁可能导致死锁或延迟，需谨慎。
        // 但这里是 Timer Task，如果其他地方持有锁并等待 Timer Task (例如 xTimerStop 阻塞)，就会死锁。
        // 
        // 鉴于 FW_SetTimer/ReleaseTimer 都使用 portMAX_DELAY 等待锁，
        // 而它们可能在任意任务调用。
        // 如果我们在 Timer Callback 中拿锁，必须非常小心。
        // 
        // 为了安全起见，我们暂不自动释放 is_allocated。
        // 让用户逻辑决定（或者像之前一样，依靠 SetTimer 的查找复用机制）。
        // 只要用户对同一个 (handler, id) 重复调用 SetTimer，就会一直复用同一个 slot，不会泄漏。
        // 只有当用户使用大量不同的 (handler, id) 且不 Release 时才会满。
    }
}

