/**
 * @file openwaifu.c
 * @brief OpenWaifu 应用主入口。
 *
 * 负责按顺序初始化各子系统：
 *   1. 日志与板级硬件（board_register_hardware）
 *   2. LVGL 图形界面（openwaifu_ui，任务清单）
 *   3. BLE 从机（openwaifu_ble，接收电脑端推送的 Agent 信息）
 *
 * 具体的 BLE 收发逻辑见 openwaifu_ble.c，界面逻辑见 openwaifu_ui.c。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#include "tuya_cloud_types.h"

#include "tal_api.h"
#include "tkl_output.h"
#include "tkl_system.h"

#include "lv_vendor.h"
#include "board_com_api.h"

#include "openwaifu_ble.h"
#include "openwaifu_ui.h"
#include "openwaifu_wakeup.h"

/**
 * @brief 应用主流程。
 */
void user_main(void)
{
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);

    /* 板级硬件注册（屏幕、触摸等） */
    board_register_hardware();

    /* 初始化 LVGL 并构建界面（任务清单），随后启动 LVGL 任务 */
    lv_vendor_init(DISPLAY_NAME);
    openwaifu_ui_init();
    lv_vendor_start(5, 1024 * 8);

    /* 初始化 BLE 从机并开始广播；失败时不影响界面显示 */
    if (openwaifu_ble_init() != OPRT_OK) {
        PR_ERR("BLE init failed, continuing without BLE");
    } else {
        PR_NOTICE("OpenWaifu BLE Peripheral initialized");
    }

    /* 仅启用端侧“你好涂鸦”关键词检测，不启动云端 AI 对话。 */
    if (openwaifu_wakeup_init() != OPRT_OK) {
        PR_ERR("Local wake word init failed, continuing without wakeup");
    }
}

/**
 * @brief main（Linux 仿真平台入口）。
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();

    while (1) {
        tal_system_sleep(500);
    }
}
#else

static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief 应用任务线程入口。
 */
static void tuya_app_thread(void *arg)
{
    (void)arg;

    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

/**
 * @brief 嵌入式平台入口：创建应用任务线程。
 */
void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param;

    memset(&thrd_param, 0, sizeof(THREAD_CFG_T));
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority   = THREAD_PRIO_1;
    thrd_param.thrdname   = "tuya_app_main";

    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
