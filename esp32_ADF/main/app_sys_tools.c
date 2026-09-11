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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "audio_mem.h"
#include "app_player.h"

static const char *TAG = "APP_SYS_TOOLS";

#define TASK_MAX_COUNT 32

typedef struct
{
    uint32_t ulRunTimeCounter;
    uint32_t xTaskNumber;
} taskData_t;

static taskData_t previousSnapshot[TASK_MAX_COUNT];
static int taskTopIndex = 0;
static uint32_t previousTotalRunTime = 0;

static taskData_t *getPreviousTaskData(uint32_t xTaskNumber)
{
    // Try to find the task in the list of tasks
    for (int i = 0; i < taskTopIndex; i++)
    {
        if (previousSnapshot[i].xTaskNumber == xTaskNumber)
        {
            return &previousSnapshot[i];
        }
    }
    // Allocate a new entry
    ESP_ERROR_CHECK(!(taskTopIndex < TASK_MAX_COUNT));
    taskData_t *result = &previousSnapshot[taskTopIndex];
    result->xTaskNumber = xTaskNumber;
    taskTopIndex++;
    return result;
}

static void _system_dump()
{
    // uint32_t totalRunTime;
    // TaskStatus_t taskStats[TASK_MAX_COUNT];
    // uint32_t taskCount = uxTaskGetSystemState(taskStats, TASK_MAX_COUNT, &totalRunTime);
    // uint32_t totalDelta = totalRunTime - previousTotalRunTime;
    // float f = 100.0 / totalDelta;
    // int task_index = 1;

    // // 修改表头格式
    // const char *header_format = 
    //     "│ %3s │ %-16s│ %12s │ %12s │ %12s │\n";
    // const char *data_format = 
    //     "│ %3d │ %-16s│ %11.2f%% │ %8lu B   │ %12lu │\n";

    // // CPU0 表头
    // printf("\n┌─────┬────────────────┬──────────────┬──────────────┬──────────────┐\n");
    // printf("│                           CPU0 Task Status Monitor                           │\n");
    // printf("├─────┼────────────────┼──────────────┼──────────────┼──────────────┤\n");
    // printf(header_format, 
    //        "No.", "Task Name", "CPU Usage", "Stack Free", "Priority");
    // printf("├─────┼────────────────┼──────────────┼──────────────┼──────────────┤\n");

    // // CPU0 任务数据
    // for (uint32_t i = 0; i < taskCount; i++)
    // {
    //     TaskStatus_t *stats = &taskStats[i];
    //     TaskHandle_t task = xTaskGetHandle(stats->pcTaskName);
    //     if (task != NULL && xTaskGetAffinity(task) == 0)
    //     {
    //         taskData_t *previousTaskData = getPreviousTaskData(stats->xTaskNumber);
    //         uint32_t taskRunTime = stats->ulRunTimeCounter;
    //         float load = f * (taskRunTime - previousTaskData->ulRunTimeCounter);
    //         UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(task);

    //         printf(data_format,
    //                task_index++,
    //                stats->pcTaskName,
    //                load,
    //                (unsigned long)highWaterMark,
    //                (unsigned long)stats->uxBasePriority
    //         );

    //         previousTaskData->ulRunTimeCounter = taskRunTime;
    //     }
    // }

    // // CPU1 表头
    // printf("├─────┼────────────────┼──────────────┼──────────────┼──────────────┼──────────┤\n");
    // printf("│                                CPU1 Task Status Monitor                                │\n");
    // printf("├─────┼────────────────┼──────────────┼──────────────┼──────────────┼──────────┤\n");
    // printf(header_format, 
    //        "No.", "Task Name", "CPU Usage", "Stack Free", "Priority");
    // printf("├─────┼────────────────┼──────────────┼──────────────┼──────────────┼──────────┤\n");

    // // CPU1 任务数据
    // for (uint32_t i = 0; i < taskCount; i++)
    // {
    //     TaskStatus_t *stats = &taskStats[i];
    //     TaskHandle_t task = xTaskGetHandle(stats->pcTaskName);
    //     if (task != NULL && xTaskGetAffinity(task) == 1)
    //     {
    //         taskData_t *previousTaskData = getPreviousTaskData(stats->xTaskNumber);
    //         uint32_t taskRunTime = stats->ulRunTimeCounter;
    //         float load = f * (taskRunTime - previousTaskData->ulRunTimeCounter);
    //         UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(task);

    //         printf(data_format,
    //                task_index++,
    //                stats->pcTaskName,
    //                load,
    //                (unsigned long)highWaterMark,
    //                (unsigned long)stats->uxBasePriority
    //         );

    //         previousTaskData->ulRunTimeCounter = taskRunTime;
    //     }
    // }
    // previousTotalRunTime = totalRunTime;

    // 内存信息
    printf("├──────────────────────────── Memory Usage Status ─────────────────────────────┤\n");
    // 内部SRAM
    printf("│ SRAM Total     : %-10lu KB                                               │\n", 
           (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_INTERNAL) / 1024));
    printf("│ SRAM Free      : %-10lu KB (%.1f%%)                                      │\n", 
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           100.0f * heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
    printf("│ SRAM Used      : %-10lu KB                                               │\n", 
           (unsigned long)((heap_caps_get_total_size(MALLOC_CAP_INTERNAL) - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)) / 1024));
    printf("│ SRAM Largest   : %-10lu KB                                               │\n", 
           (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024));
    
    // PSRAM (如果可用)
    #ifdef CONFIG_SPIRAM
    printf("├────────────────────────────── PSRAM Status ───────────────────────────────┤\n");
    printf("│ PSRAM Total    : %-10lu KB                                               │\n", 
           (unsigned long)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024));
    printf("│ PSRAM Free     : %-10lu KB (%.1f%%)                                      │\n", 
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
           100.0f * heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    printf("│ PSRAM Used     : %-10lu KB                                               │\n", 
           (unsigned long)((heap_caps_get_total_size(MALLOC_CAP_SPIRAM) - heap_caps_get_free_size(MALLOC_CAP_SPIRAM)) / 1024));
    printf("│ PSRAM Largest  : %-10lu KB                                               │\n", 
           (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    #endif
    
    // 总体内存状况
    printf("├─────────────────────────── Overall Memory Status ────────────────────────┤\n");
    printf("│ Total Heap Free : %-10lu KB                                              │\n", 
           (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_DEFAULT) / 1024));
    printf("│ Largest Block   : %-10lu KB                                              │\n", 
           (unsigned long)(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) / 1024));
    printf("│ Minimum Free    : %-10lu KB                                              │\n", 
           (unsigned long)(heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) / 1024));
    printf("└─────────────────────────────────────────────────────────────────────────┘\n");

}
void sys_monitor_task(void *para)
{
    while (1) {
        vTaskDelay(10000 / portTICK_PERIOD_MS);
        audio_player_show_element_status();
        AUDIO_MEM_SHOW(TAG);
       //  _system_dump();
#ifdef CONFIG_FREERTOS_USE_TRACE_FACILITY
        // static char buf[2048];
        // vTaskList(buf);
        // printf("Task List:\nTask Name    Status   Prio    HWM    Task Number\n%s\n", buf);
#endif
    }
    vTaskDelete(NULL);
}

void start_sys_monitor(void)
{
    
    xTaskCreatePinnedToCore(sys_monitor_task, "sys_monitor_task", (4 * 1024), NULL, 1, NULL, 1);   
}