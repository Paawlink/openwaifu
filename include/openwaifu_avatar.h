/**
 * @file openwaifu_avatar.h
 * @brief 左栏虚拟形象动画（基于 LVGL lv_animimg 的帧序列播放）。
 *
 * 设计目标：把“虚拟形象”抽象成一个可按状态切换的帧序列播放器。
 * 当前仅内置“思考/加载”序列（火焰角色，thinking_01..08）；后续可为不同
 * 状态（编码、测试、出错、空闲等）登记各自的帧序列，调用方只需通过
 * openwaifu_avatar_set_state() 切换，无需关心底层帧资源与动画细节。
 */

#ifndef OPENWAIFU_AVATAR_H
#define OPENWAIFU_AVATAR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 虚拟形象动画状态。
 *
 * 分为三类：
 * - 工作状态（THINKING / CODING / CYCLING）：有任务时随机选用其一。
 * - 空闲状态（MOYU / SLEEP）：无任务时随机选用其一。
 * - 事件状态（ERROR / CELEBRATION / CONFUSED）：事件触发时临时展示约 5 秒，
 *   随后自动回落到之前的工作/空闲状态。
 */
typedef enum {
    OPENWAIFU_AVATAR_THINKING = 0, /* 思考 / 加载中 */
    OPENWAIFU_AVATAR_CODING,       /* 编码中 */
    OPENWAIFU_AVATAR_CYCLING,      /* 骑行（工作中的趣味形象） */
    OPENWAIFU_AVATAR_ERROR,        /* 出错（事件） */
    OPENWAIFU_AVATAR_CELEBRATION,  /* 庆祝（任务完成事件） */
    OPENWAIFU_AVATAR_CONFUSED,     /* 困惑（用户取消事件） */
    OPENWAIFU_AVATAR_MOYU,         /* 摸鱼（空闲） */
    OPENWAIFU_AVATAR_SLEEP,        /* 睡觉（空闲） */
    OPENWAIFU_AVATAR_STATE_MAX,
} openwaifu_avatar_state_t;

/**
 * 在 parent 内创建虚拟形象动画对象（lv_animimg）。
 * 创建后默认播放 THINKING 序列并无限循环。
 * @param parent 父容器
 * @return 创建的 animimg 对象（失败返回 NULL）
 */
lv_obj_t *openwaifu_avatar_create(lv_obj_t *parent);

/**
 * 切换虚拟形象当前播放的帧序列（按状态）。
 *
 * 切换时带有淡入淡出过渡效果：先将不透明度从 255 渐变到 0（约 200 ms），
 * 替换帧序列后再从 0 渐变回 255（约 200 ms），使形象切换更加自然。
 * 若目标状态未登记独立序列，则回退到 THINKING 序列；与当前状态相同则不重启动画。
 * @param state 目标状态
 */
void openwaifu_avatar_set_state(openwaifu_avatar_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* OPENWAIFU_AVATAR_H */
