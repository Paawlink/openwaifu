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
 * 为“不同状态播放不同帧序列”预留扩展位：新增状态时在此追加枚举，并在
 * openwaifu_avatar.c 的序列表中登记对应帧资源即可；未登记的状态会自动
 * 回退到 THINKING 序列，保证行为安全。
 */
typedef enum {
    OPENWAIFU_AVATAR_THINKING = 0, /* 思考 / 加载中（当前唯一有独立美术资源的状态） */
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
 * 若目标状态未登记独立序列，则回退到 THINKING 序列；与当前状态相同则不重启动画。
 * @param state 目标状态
 */
void openwaifu_avatar_set_state(openwaifu_avatar_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* OPENWAIFU_AVATAR_H */
