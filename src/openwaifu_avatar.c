/**
 * @file openwaifu_avatar.c
 * @brief 左栏虚拟形象帧序列播放器实现（LVGL lv_animimg）。
 *
 * 帧资源由 src/assets/thinking_0X.c 提供（RGB565A8，135x185，由 LVGLImage.py
 * 转换而来）。此处仅以“状态 -> 帧序列”的表驱动方式管理播放，便于后续为不同
 * 状态登记各自的帧序列。
 */

#include "openwaifu_avatar.h"

/* 思考/加载序列：火焰角色 8 帧，符号定义在 src/assets/thinking_0X.c 中。 */
extern const lv_image_dsc_t thinking_01;
extern const lv_image_dsc_t thinking_02;
extern const lv_image_dsc_t thinking_03;
extern const lv_image_dsc_t thinking_04;
extern const lv_image_dsc_t thinking_05;
extern const lv_image_dsc_t thinking_06;
extern const lv_image_dsc_t thinking_07;
extern const lv_image_dsc_t thinking_08;

/* 帧尺寸（与美术资源一致），用于给 animimg 定尺寸。 */
#define AVATAR_FRAME_W 135
#define AVATAR_FRAME_H 185

/** 单个帧序列描述：帧指针数组 + 帧数 + 整段动画播放一遍的时长(ms)。 */
typedef struct {
    const void **frames;      /* lv_image_dsc_t* 数组（lv_animimg_set_src 需要 const void*[]） */
    uint8_t      frame_count; /* 帧数 */
    uint32_t     duration_ms; /* 播放一遍的时长，帧率 = frame_count / (duration_ms/1000) */
} avatar_seq_t;

static const void *sg_thinking_frames[] = {
    &thinking_01, &thinking_02, &thinking_03, &thinking_04,
    &thinking_05, &thinking_06, &thinking_07, &thinking_08,
};

/* 状态 -> 序列表：后续新增状态只需在此登记对应帧序列（未登记者回退到 THINKING）。 */
static const avatar_seq_t sg_sequences[OPENWAIFU_AVATAR_STATE_MAX] = {
    [OPENWAIFU_AVATAR_THINKING] =
        {
            .frames      = sg_thinking_frames,
            .frame_count = 8,
            .duration_ms = 960, /* 8 帧约 8.3fps，思考/加载观感平稳 */
        },
};

static lv_obj_t               *sg_animimg = NULL;
static openwaifu_avatar_state_t sg_state  = OPENWAIFU_AVATAR_STATE_MAX; /* 未初始化标记 */

/** 应用某状态对应的帧序列并（重新）启动循环动画。 */
static void __apply_sequence(openwaifu_avatar_state_t state)
{
    const avatar_seq_t *seq;

    if (sg_animimg == NULL) {
        return;
    }
    if (state >= OPENWAIFU_AVATAR_STATE_MAX) {
        state = OPENWAIFU_AVATAR_THINKING;
    }

    /* 未登记独立序列的状态回退到思考序列，保证任何状态都有画面。 */
    seq = &sg_sequences[state];
    if (seq->frames == NULL || seq->frame_count == 0) {
        state = OPENWAIFU_AVATAR_THINKING;
        seq   = &sg_sequences[OPENWAIFU_AVATAR_THINKING];
    }

    /* 状态未变则不重启动画，避免画面回跳。 */
    if (state == sg_state) {
        return;
    }
    sg_state = state;

    lv_animimg_set_src(sg_animimg, seq->frames, seq->frame_count);
    lv_animimg_set_duration(sg_animimg, seq->duration_ms);
    lv_animimg_set_repeat_count(sg_animimg, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(sg_animimg);
}

lv_obj_t *openwaifu_avatar_create(lv_obj_t *parent)
{
    sg_animimg = lv_animimg_create(parent);
    if (sg_animimg == NULL) {
        return NULL;
    }

    /* animimg 继承自 image：去掉主题默认样式，仅按帧尺寸占位、居中显示。 */
    lv_obj_remove_style_all(sg_animimg);
    lv_obj_set_size(sg_animimg, AVATAR_FRAME_W, AVATAR_FRAME_H);
    lv_obj_center(sg_animimg);

    sg_state = OPENWAIFU_AVATAR_STATE_MAX; /* 强制首次应用序列 */
    __apply_sequence(OPENWAIFU_AVATAR_THINKING);
    return sg_animimg;
}

void openwaifu_avatar_set_state(openwaifu_avatar_state_t state)
{
    __apply_sequence(state);
}
