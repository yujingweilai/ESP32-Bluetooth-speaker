/**
 * @file main_test.c
 * @brief 针对 fw_timer 模块的综合测试程序
 * @details
 * - 确保您的项目中已包含 FreeRTOS 内核。
 * - 确保项目中已添加 fw_timer.h 和 fw_timer.c。
 * - 确保 printf 已重定向到 UART 或调试控制台。
 * - 在 FreeRTOSConfig.h 中：
 * #define configUSE_TIMERS 1
 * #define configTIMER_SERVICE_TASK_PRIORITY (configMAX_PRIORITIES - 1)
 * #define configTIMER_TASK_STACK_DEPTH (configMINIMAL_STACK_SIZE * 2)
 * #define configTIMER_QUEUE_LENGTH 10
 */

#include <stdio.h>
// 引入我们要测试的定时器模块头文件
#include "fw_timer.h"

/* 定义测试任务的优先级和栈大小 */
#define TEST_TASK_PRIORITY      ( tskIDLE_PRIORITY + 1 )
#define TEST_TASK_STACK_SIZE    ( configMINIMAL_STACK_SIZE * 4 )

// === 声明几个不同的回调函数，模拟不同业务模块 ===

// 测试回调函数 1
static void test_callback_1(U16 timerId, void *msg) {
    char *message = (char *)msg;
    // TickType_t xTickCount = xTaskGetTickCount();
    // printf("Tick: %lu, ", xTickCount);
    printf("--> [回调函数 1] 触发! ID: %d, 消息: \"%s\"\n", timerId, message);
}

// 测试回调函数 2
static void test_callback_2(U16 timerId, void *msg) {
    int *value = (int *)msg;
    // TickType_t xTickCount = xTaskGetTickCount();
    // printf("Tick: %lu, ", xTickCount);
    printf("--> [回调函数 2] 触发! ID: %d, 整数值: %d\n", timerId, *value);
}

// 测试回调函数 3 (用于资源耗尽测试)
static void dummy_callback(U16 timerId, void *msg) {
    // 这个回调在测试中不会被触发，只是作为一个唯一的函数指针使用
}


/**
 * @brief 定时器测试的核心任务
 */
static void vTimerTestTask(void *pvParameters) {
    static int test_integer = 12345;
    vTaskDelay(pdMS_TO_TICKS(5000));
    printf("\n\n--- 定时器模块综合测试开始 ---\n");
    printf("说明: 测试程序将按顺序演示各项功能。\n");

    // --- 测试 1: 基本功能和不同回调使用相同ID ---
    printf("\n--- [测试 1] ---\n");
    printf("设置多个定时器，包括两个使用相同ID(101)但回调函数不同的定时器。\n");
    printf("预期: 定时器将按延迟时间顺序触发，并且ID为101的两个定时器都会正确触发。\n");

    FW_SetTimer(test_callback_1, 101, "来自回调1的消息", 2000); // 2秒后
    FW_SetTimer(test_callback_2, 201, &test_integer, 3000);   // 3秒后
    FW_SetTimer(test_callback_1, 102, "这是另一个消息", 4000); // 4秒后
    FW_SetTimer(test_callback_2, 101, &test_integer, 5000);   // 5秒后 (与第一个ID相同)

    printf("已设置4个定时器，请等待5秒观察结果...\n");
    vTaskDelay(pdMS_TO_TICKS(6000)); // 等待所有定时器触发


    // --- 测试 2: 释放一个未到期的定时器 ---
    printf("\n--- [测试 2] ---\n");
    printf("设置一个3秒后触发的定时器，然后在1秒后将其释放。\n");
    printf("预期: 该定时器永远不会触发。\n");

    FW_SetTimer(test_callback_1, 301, "这条消息永远不应出现!", 3000);
    printf("已设置 ID=301 的定时器 (3秒后触发)。\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    FW_ReleaseTimer(test_callback_1, 301);
    printf("已释放 ID=301 的定时器。等待3秒确认它不会触发...\n");
    vTaskDelay(pdMS_TO_TICKS(3000));


    // --- 测试 3: 重置一个正在运行的定时器 ---
    printf("\n--- [测试 3] ---\n");
    printf("设置一个3秒后触发的定时器，1秒后用一个新的延迟(5秒)重新设置它。\n");
    printf("预期: 定时器将在重新设置后的5秒触发，而不是原来的3秒。\n");

    FW_SetTimer(test_callback_2, 401, &test_integer, 3000);
    printf("已设置 ID=401 的定时器 (3秒后触发)。\n");
    vTaskDelay(pdMS_TO_TICKS(1000));
    printf("1秒后，使用新的延迟(5秒)重置 ID=401 的定时器...\n");
    FW_SetTimer(test_callback_2, 401, &test_integer, 5000);
    printf("等待6秒观察结果...\n");
    vTaskDelay(pdMS_TO_TICKS(6000));


    // --- 测试 4: 资源耗尽测试 ---
    printf("\n--- [测试 4] ---\n");
    printf("尝试创建超过 FW_MAX_TIMERS (%d个) 上限的定时器。\n", FW_MAX_TIMERS);
    printf("预期: 第 %d 个定时器设置成功，第 %d 个会失败并返回 pdFAIL。\n", FW_MAX_TIMERS, FW_MAX_TIMERS + 1);
    
    S8 result = pdPASS;
    int i;
    for (i = 0; i < FW_MAX_TIMERS; i++) {
        // 使用不同的 timerId 来确保它们是独立的定时器
        result = FW_SetTimer(dummy_callback, (U16)(500 + i), NULL, 10000);
        if (result == pdFAIL) {
            printf("错误: 在创建第 %d 个定时器时意外失败！\n", i + 1);
            break;
        }
    }

    if (result == pdPASS) {
        printf("成功创建了 %d 个定时器。\n", FW_MAX_TIMERS);
        printf("现在尝试创建第 %d 个 (预期会失败)...\n", FW_MAX_TIMERS + 1);
        result = FW_SetTimer(dummy_callback, (U16)(500 + i), NULL, 10000);
        if (result == pdFAIL) {
            printf("测试成功: 创建第 %d 个定时器时，FW_SetTimer 正确地返回了 pdFAIL。\n", FW_MAX_TIMERS + 1);
        } else {
            printf("测试失败: 竟然成功创建了超过上限的定时器！\n");
        }
    }
    
    // 清理本次测试创建的所有定时器
    printf("清理资源中...\n");
    for (i = 0; i < FW_MAX_TIMERS; i++) {
        FW_ReleaseTimer(dummy_callback, (U16)(500 + i));
    }


    // --- 测试结束 ---
    printf("\n--- 所有测试已完成 ---\n");
    vTaskDelete(NULL); // 测试完成，删除任务自身
}


int fw_timer_test(void) {
    // (此处应有您的硬件初始化代码，例如时钟、串口等)
    // hardware_init();
    // uart_init_for_printf();

    // 初始化我们封装的定时器模块
    if (FW_TimerInit() != pdPASS) {
        // printf("FATAL: FW_TimerInit() failed!\n");
        while(1);
    }

    // 创建测试任务
    xTaskCreate(vTimerTestTask, "TimerTest", TEST_TASK_STACK_SIZE, NULL, TEST_TASK_PRIORITY, NULL);
    return 0;


}
