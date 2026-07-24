/**
 * @file openwaifu_ui.h
 * @brief OpenWaifu LVGL 界面模块接口（像素风任务清单）。
 *
 * 界面采用横屏布局（480x320），分为左右两栏：左栏为虚拟形象占位卡片（内容留空），
 * 右栏以“每个活跃会话固定占一行卡片”的方式实时展示来自 BLE（守护进程推送的 Agent
 * 会话）的状态。行内自左到右为「插件图标」「任务主题（行主体）」「状态指示灯」，并按
 * 会话 ID（Topic）稳定排序以避免顺序反复跳动。状态指示灯与底部图例一一对应：运行中
 * （spinner）、已完成（绿点）、异常（红点）、已查看（灰点）。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#ifndef __OPENWAIFU_UI_H__
#define __OPENWAIFU_UI_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 构建 LVGL 界面并启动刷新定时器。
 *
 * 必须在 LVGL 初始化（lv_vendor_init）之后调用。定时器会周期性从 BLE 模块
 * 拉取命令行并按需重建任务清单。
 */
void openwaifu_ui_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_UI_H__ */
