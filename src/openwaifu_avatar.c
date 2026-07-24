/**
 * @file openwaifu_avatar.c
 * @brief 左栏虚拟形象帧序列播放器实现（LVGL lv_animimg）。
 *
 * 帧资源由 src/assets/<emotion>_NN.c 提供（内嵌 PNG，135x185）。此处以
 * "状态 -> 帧序列"的表驱动方式管理播放，并为每次状态切换添加淡入淡出
 * 过渡效果，使形象切换更加自然。
 *
 * 形象分类：
 * - 工作状态：Thinking(8帧) / Coding(8帧) / Cycling(6帧)
 * - 空闲状态：Moyu(6帧) / Sleep(4帧)
 * - 事件状态：Error(7帧) / Celebration(8帧) / Confused(8帧)
 */

#include "openwaifu_avatar.h"

/* ── 帧资源声明 ────────────────────────────────────────── */

/* Thinking（思考/加载）：8 帧 */
LV_IMAGE_DECLARE(thinking_01);
LV_IMAGE_DECLARE(thinking_02);
LV_IMAGE_DECLARE(thinking_03);
LV_IMAGE_DECLARE(thinking_04);
LV_IMAGE_DECLARE(thinking_05);
LV_IMAGE_DECLARE(thinking_06);
LV_IMAGE_DECLARE(thinking_07);
LV_IMAGE_DECLARE(thinking_08);

/* Coding（编码）：8 帧 */
LV_IMAGE_DECLARE(coding_01);
LV_IMAGE_DECLARE(coding_02);
LV_IMAGE_DECLARE(coding_03);
LV_IMAGE_DECLARE(coding_04);
LV_IMAGE_DECLARE(coding_05);
LV_IMAGE_DECLARE(coding_06);
LV_IMAGE_DECLARE(coding_07);
LV_IMAGE_DECLARE(coding_08);

/* Cycling（骑行）：6 帧 */
LV_IMAGE_DECLARE(cycling_01);
LV_IMAGE_DECLARE(cycling_02);
LV_IMAGE_DECLARE(cycling_03);
LV_IMAGE_DECLARE(cycling_04);
LV_IMAGE_DECLARE(cycling_05);
LV_IMAGE_DECLARE(cycling_06);

/* Error（出错）：7 帧（原始素材编号缺少 frame_04） */
LV_IMAGE_DECLARE(error_01);
LV_IMAGE_DECLARE(error_02);
LV_IMAGE_DECLARE(error_03);
LV_IMAGE_DECLARE(error_04);
LV_IMAGE_DECLARE(error_05);
LV_IMAGE_DECLARE(error_06);
LV_IMAGE_DECLARE(error_07);

/* Celebration（庆祝）：8 帧 */
LV_IMAGE_DECLARE(celebration_01);
LV_IMAGE_DECLARE(celebration_02);
LV_IMAGE_DECLARE(celebration_03);
LV_IMAGE_DECLARE(celebration_04);
LV_IMAGE_DECLARE(celebration_05);
LV_IMAGE_DECLARE(celebration_06);
LV_IMAGE_DECLARE(celebration_07);
LV_IMAGE_DECLARE(celebration_08);

/* Confused（困惑）：8 帧 */
LV_IMAGE_DECLARE(confused_01);
LV_IMAGE_DECLARE(confused_02);
LV_IMAGE_DECLARE(confused_03);
LV_IMAGE_DECLARE(confused_04);
LV_IMAGE_DECLARE(confused_05);
LV_IMAGE_DECLARE(confused_06);
LV_IMAGE_DECLARE(confused_07);
LV_IMAGE_DECLARE(confused_08);

/* Moyu（摸鱼）：6 帧 */
LV_IMAGE_DECLARE(moyu_01);
LV_IMAGE_DECLARE(moyu_02);
LV_IMAGE_DECLARE(moyu_03);
LV_IMAGE_DECLARE(moyu_04);
LV_IMAGE_DECLARE(moyu_05);
LV_IMAGE_DECLARE(moyu_06);

/* Sleep（睡觉）：4 帧 */
LV_IMAGE_DECLARE(sleep_01);
LV_IMAGE_DECLARE(sleep_02);
LV_IMAGE_DECLARE(sleep_03);
LV_IMAGE_DECLARE(sleep_04);

/* ── 常量 ──────────────────────────────────────────────── */

/* 帧尺寸（与美术资源一致），用于给 animimg 定尺寸。 */
#define AVATAR_FRAME_W 135
#define AVATAR_FRAME_H 185

/* 淡入淡出过渡时长（ms） */
#define AVATAR_FADE_DURATION_MS 200

/* ── 类型 ──────────────────────────────────────────────── */

/** 单个帧序列描述：帧指针数组 + 帧数 + 整段动画播放一遍的时长(ms)。 */
typedef struct {
    const void **frames;      /* lv_image_dsc_t* 数组（lv_animimg_set_src 需要 const void*[]） */
    uint8_t      frame_count; /* 帧数 */
    uint32_t     duration_ms; /* 播放一遍的时长，帧率 = frame_count / (duration_ms/1000) */
} avatar_seq_t;

/* ── 帧数组 ────────────────────────────────────────────── */

static const void *sg_thinking_frames[] = {
    &thinking_01, &thinking_02, &thinking_03, &thinking_04,
    &thinking_05, &thinking_06, &thinking_07, &thinking_08,
};

static const void *sg_coding_frames[] = {
    &coding_01, &coding_02, &coding_03, &coding_04,
    &coding_05, &coding_06, &coding_07, &coding_08,
};

static const void *sg_cycling_frames[] = {
    &cycling_01, &cycling_02, &cycling_03,
    &cycling_04, &cycling_05, &cycling_06,
};

static const void *sg_error_frames[] = {
    &error_01, &error_02, &error_03, &error_04,
    &error_05, &error_06, &error_07,
};

static const void *sg_celebration_frames[] = {
    &celebration_01, &celebration_02, &celebration_03, &celebration_04,
    &celebration_05, &celebration_06, &celebration_07, &celebration_08,
};

static const void *sg_confused_frames[] = {
    &confused_01, &confused_02, &confused_03, &confused_04,
    &confused_05, &confused_06, &confused_07, &confused_08,
};

static const void *sg_moyu_frames[] = {
    &moyu_01, &moyu_02, &moyu_03,
    &moyu_04, &moyu_05, &moyu_06,
};

static const void *sg_sleep_frames[] = {
    &sleep_01, &sleep_02, &sleep_03, &sleep_04,
};

/* ── 状态 -> 序列表 ────────────────────────────────────── */

static const avatar_seq_t sg_sequences[OPENWAIFU_AVATAR_STATE_MAX] = {
    [OPENWAIFU_AVATAR_THINKING] = {
        .frames      = sg_thinking_frames,
        .frame_count = 8,
        .duration_ms = 960,  /* ~8.3fps */
    },
    [OPENWAIFU_AVATAR_CODING] = {
        .frames      = sg_coding_frames,
        .frame_count = 8,
        .duration_ms = 960,
    },
    [OPENWAIFU_AVATAR_CYCLING] = {
        .frames      = sg_cycling_frames,
        .frame_count = 6,
        .duration_ms = 720,
    },
    [OPENWAIFU_AVATAR_ERROR] = {
        .frames      = sg_error_frames,
        .frame_count = 7,
        .duration_ms = 700,  /* 略快，故障感 */
    },
    [OPENWAIFU_AVATAR_CELEBRATION] = {
        .frames      = sg_celebration_frames,
        .frame_count = 8,
        .duration_ms = 960,
    },
    [OPENWAIFU_AVATAR_CONFUSED] = {
        .frames      = sg_confused_frames,
        .frame_count = 8,
        .duration_ms = 1200, /* 略慢，困惑感 */
    },
    [OPENWAIFU_AVATAR_MOYU] = {
        .frames      = sg_moyu_frames,
        .frame_count = 6,
        .duration_ms = 1200, /* 慢节奏，悠闲 */
    },
    [OPENWAIFU_AVATAR_SLEEP] = {
        .frames      = sg_sleep_frames,
        .frame_count = 4,
        .duration_ms = 1200, /* 很慢，困倦 */
    },
};

/* ── 模块状态 ──────────────────────────────────────────── */

static lv_obj_t                *sg_animimg         = NULL;
static openwaifu_avatar_state_t sg_state           = OPENWAIFU_AVATAR_STATE_MAX; /* 当前播放的状态 */
static openwaifu_avatar_state_t sg_pending_state   = OPENWAIFU_AVATAR_STATE_MAX; /* 淡出后要切换到的状态 */
static bool                     sg_is_transitioning = false;                     /* 正在淡入淡出中 */

/* ── 淡入淡出动画 ──────────────────────────────────────── */

/** 淡出/淡入执行回调：设置 animimg 的不透明度。 */
static void __fade_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

/** 淡出完成回调：切换帧序列并启动淡入。 */
static void __fade_out_ready_cb(lv_anim_t *a)
{
    const avatar_seq_t *seq;
    openwaifu_avatar_state_t target = sg_pending_state;
    lv_anim_t fade_in;

    if (sg_animimg == NULL) {
        sg_is_transitioning = false;
        return;
    }

    /* 安全回退 */
    if (target >= OPENWAIFU_AVATAR_STATE_MAX) {
        target = OPENWAIFU_AVATAR_THINKING;
    }
    seq = &sg_sequences[target];
    if (seq->frames == NULL || seq->frame_count == 0) {
        target = OPENWAIFU_AVATAR_THINKING;
        seq    = &sg_sequences[OPENWAIFU_AVATAR_THINKING];
    }

    /* 切换帧序列 */
    sg_state = target;
    lv_animimg_set_src(sg_animimg, seq->frames, seq->frame_count);
    lv_animimg_set_duration(sg_animimg, seq->duration_ms);
    lv_animimg_set_repeat_count(sg_animimg, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(sg_animimg);

    /* 启动淡入动画 0 -> 255 */
    lv_anim_init(&fade_in);
    lv_anim_set_var(&fade_in, sg_animimg);
    lv_anim_set_exec_cb(&fade_in, __fade_opa_cb);
    lv_anim_set_values(&fade_in, 0, 255);
    lv_anim_set_duration(&fade_in, AVATAR_FADE_DURATION_MS);
    lv_anim_set_path_cb(&fade_in, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&fade_in, NULL); /* 淡入结束后无需额外操作 */
    lv_anim_start(&fade_in);

    (void)a;
    sg_is_transitioning = false;
}

/* ── 内部实现 ──────────────────────────────────────────── */

/** 直接切换帧序列（无过渡），用于初始化。 */
static void __do_apply_sequence(openwaifu_avatar_state_t state)
{
    const avatar_seq_t *seq;

    if (sg_animimg == NULL) {
        return;
    }
    if (state >= OPENWAIFU_AVATAR_STATE_MAX) {
        state = OPENWAIFU_AVATAR_THINKING;
    }

    seq = &sg_sequences[state];
    if (seq->frames == NULL || seq->frame_count == 0) {
        state = OPENWAIFU_AVATAR_THINKING;
        seq   = &sg_sequences[OPENWAIFU_AVATAR_THINKING];
    }

    sg_state = state;
    lv_animimg_set_src(sg_animimg, seq->frames, seq->frame_count);
    lv_animimg_set_duration(sg_animimg, seq->duration_ms);
    lv_animimg_set_repeat_count(sg_animimg, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(sg_animimg);
}

/**
 * 切换虚拟形象状态，带淡入淡出过渡。
 *
 * 若正在过渡中，仅更新 pending 状态（淡出完成后会切换到最新目标）。
 * 若目标与当前相同则不做任何操作。
 */
static void __apply_sequence(openwaifu_avatar_state_t state)
{
    lv_anim_t fade_out;

    if (sg_animimg == NULL) {
        return;
    }
    if (state >= OPENWAIFU_AVATAR_STATE_MAX) {
        state = OPENWAIFU_AVATAR_THINKING;
    }

    /* 安全回退检查 */
    {
        const avatar_seq_t *seq = &sg_sequences[state];
        if (seq->frames == NULL || seq->frame_count == 0) {
            state = OPENWAIFU_AVATAR_THINKING;
        }
    }

    /* 状态未变则不重启动画 */
    if (state == sg_state && !sg_is_transitioning) {
        return;
    }

    /* 正在过渡中：更新目标，让正在进行的淡出结束后直接切换到新目标 */
    if (sg_is_transitioning) {
        sg_pending_state = state;
        return;
    }

    sg_pending_state    = state;
    sg_is_transitioning = true;

    /* 启动淡出动画 255 -> 0 */
    lv_anim_init(&fade_out);
    lv_anim_set_var(&fade_out, sg_animimg);
    lv_anim_set_exec_cb(&fade_out, __fade_opa_cb);
    lv_anim_set_values(&fade_out, 255, 0);
    lv_anim_set_duration(&fade_out, AVATAR_FADE_DURATION_MS);
    lv_anim_set_path_cb(&fade_out, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&fade_out, __fade_out_ready_cb);
    lv_anim_start(&fade_out);
}

/* ── 对外接口 ──────────────────────────────────────────── */

lv_obj_t *openwaifu_avatar_create(lv_obj_t *parent)
{
    sg_animimg = lv_animimg_create(parent);
    if (sg_animimg == NULL) {
        return NULL;
    }

    /* animimg 继承自 image：去掉主题默认样式，仅按帧尺寸占位、居中显示。 */
    lv_obj_remove_style_all(sg_animimg);
    lv_obj_set_size(sg_animimg, AVATAR_FRAME_W, AVATAR_FRAME_H);
    lv_obj_align(sg_animimg, LV_ALIGN_CENTER, 0, -12);

    /* 首次直接应用，无需淡入淡出 */
    sg_state           = OPENWAIFU_AVATAR_STATE_MAX; /* 强制首次应用 */
    sg_pending_state   = OPENWAIFU_AVATAR_STATE_MAX;
    sg_is_transitioning = false;
    __do_apply_sequence(OPENWAIFU_AVATAR_THINKING);
    return sg_animimg;
}

void openwaifu_avatar_set_state(openwaifu_avatar_state_t state)
{
    __apply_sequence(state);
}
