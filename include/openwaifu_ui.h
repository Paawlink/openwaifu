/**
 * @file openwaifu_ui.h
 * @brief OpenWaifu LVGL 界面模块接口（会话看板 + 情绪状态机）。
 *
 * 界面采用横屏布局（480x320），以“每个活跃会话固定占一行/一张卡片”的方式实时展示
 * 来自 BLE（守护进程推送的 Agent 会话）的状态。列表中任务主题为行主体，状态与已运行
 * 时长靠右显示，并按会话 ID（Topic）稳定排序以避免顺序反复跳动；同时根据活跃会话数量
 * 切换设备“情绪”：0=睡觉中、1=摸鱼中（单任务大卡片）、2-3=认真搬砖、4-6=火力全开、>6=要炸了（列表）。
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
 * 拉取命令行并更新会话看板，同时在本地按秒刷新每个会话的运行时长。
 */
void openwaifu_ui_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_UI_H__ */
