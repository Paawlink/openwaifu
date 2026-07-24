/**
 * @file openwaifu_ui.c
 * @brief OpenWaifu 会话看板 UI（LVGL 9）——像素风“任务清单”布局。
 *
 * 界面采用横屏布局（480x320），整体分为左右两栏：
 *
 *   左栏：虚拟形象占位卡片（本期仅保留白色圆角外框，内容留空待后续填充）。
 *   右栏：任务清单 + 底部图例。每个活跃会话固定占一行卡片，行内自左到右为
 *         「插件图标」「任务主题（主体，占据剩余空间）」「状态指示灯」。
 *
 * 状态指示灯用颜色/动画区分四种状态（与底部图例一一对应）：
 *   运行中 -> 旋转指示器（spinner）
 *   已完成 -> 绿色圆点
 *   异常   -> 红色圆点
 *   已查看 -> 灰色圆点（保留语义，当前由守护进程状态派生时不会触发）
 *
 * 为避免会话增删导致顺序反复跳动，列表按会话 ID（Topic）稳定排序。
 *
 * 数据来源：BLE 模块按 FIFO 暂存来自守护进程的命令行，UI 定时器逐条取出解析。
 * 命令按前缀区分：
 *   S|<sid>|<st>|<elapsed>|<plugin>|<task>  新增或更新某会话
 *   B                                  快照同步开始（把现有会话标记为“未见”）
 *   E                                  快照同步结束（移除本轮未再出现的会话）
 *   G|<ev>|<detail>                    全局事件（本布局不展示，忽略）
 * 其中 <st> 为单字符状态码：T=思考 C=编码 V=测试 E=出错 I=空闲(完成)。
 * B/E 用于周期性快照对账，使屏幕列表无闪烁地收敛到与守护进程状态完全一致。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#include "openwaifu_ui.h"

#include <string.h>

#include "lvgl.h"
#include "openwaifu_avatar.h"
#include "openwaifu_ble.h"
#include "openwaifu_font.h"

/***********************************************************
 *************************宏定义****************************
 ***********************************************************/
/** 最多同时展示的会话数（超过后新会话将被丢弃，不入表）。 */
#define OPENWAIFU_UI_MAX_SESSIONS 8
/** 会话 ID / 插件名 / 任务文本的本地缓存长度。 */
#define OPENWAIFU_UI_SID_LEN      24
#define OPENWAIFU_UI_PLUGIN_LEN   16
#define OPENWAIFU_UI_TASK_LEN     96
/** UI 刷新定时器周期（毫秒）：用于拉取 BLE 命令并按需重建列表。 */
#define OPENWAIFU_UI_REFRESH_MS   500

/* 配色（浅色像素风主题：奶油色背景 + 白色描边卡片） */
#define COL_BG          0xF2ECE1 /* 页面背景（奶油色） */
#define COL_CARD        0xFFFFFF /* 卡片背景（白） */
#define COL_CARD_BORDER 0x2B2B2B /* 卡片描边（近黑） */
#define COL_TITLE       0x1F1F1F /* 任务主题文字 */
#define COL_TEXT_SUB    0x8A857C /* 次要文字（图例 / 空状态提示） */

/* 状态指示灯配色 */
#define COL_RUNNING 0x4A4A4A /* 运行中（spinner 前景） */
#define COL_DONE    0x5C9A3A /* 已完成（绿） */
#define COL_ERROR   0xE23B2E /* 异常（红） */
#define COL_VIEWED  0x9E9A93 /* 已查看（灰） */
#define COL_TRACK   0xD9D2C6 /* spinner 轨道（浅灰） */

/* 插件图标配色（按 plugin_type 区分） */
#define COL_ICON_CLAUDE 0xF26522 /* claudecode（橙） */
#define COL_ICON_CODEX  0x10A37F /* codex（绿） */
#define COL_ICON_OPEN   0x3B82C4 /* opencode（蓝） */
#define COL_ICON_AGENT  0xC9B29A /* 默认 / agent（米色） */

/***********************************************************
 ***********************类型定义****************************
 ***********************************************************/
/** 会话运行状态（由 BLE 单字符状态码解析而来）。 */
typedef enum {
    ST_THINKING,
    ST_CODING,
    ST_TESTING,
    ST_ERROR,
    ST_IDLE,
} ui_status_t;

/** 状态指示灯的可视档位（与底部图例一一对应）。 */
typedef enum {
    VIS_RUNNING, /* 运行中：spinner */
    VIS_DONE,    /* 已完成：绿点 */
    VIS_ERROR,   /* 异常：红点 */
    VIS_VIEWED,  /* 已查看：灰点 */
} ui_vis_t;

/** 单个会话的本地状态与其绑定的 LVGL 标签。 */
typedef struct {
    bool        used;
    bool        seen;  /* 快照同步标记：B 置为 false，S 置为 true，E 时 false 则移除 */
    char        sid[OPENWAIFU_UI_SID_LEN];
    char        plugin[OPENWAIFU_UI_PLUGIN_LEN];
    char        task[OPENWAIFU_UI_TASK_LEN];
    ui_status_t status;
    bool        done;        /* 收到 idle/完成后置位 */
    ui_vis_t    vis;         /* 缓存的可视档位，用于判断是否需要重建行 */
    lv_obj_t   *title_label; /* 任务主题标签（可就地更新文本） */
} ui_session_t;

/***********************************************************
 ***********************变量定义****************************
 ***********************************************************/
static ui_session_t sg_sessions[OPENWAIFU_UI_MAX_SESSIONS];
static lv_obj_t    *sg_list            = NULL; /* 右栏任务清单容器（按会话增删重建） */
static lv_obj_t    *sg_legend          = NULL; /* 底部图例卡片（按连接状态刷新内容） */
static bool         sg_conn_last       = false; /* 上次已展示的蓝牙连接状态 */
static bool         sg_structure_dirty = true; /* 会话增删或状态档位变化时需重建列表 */

/***********************************************************
 ***********************工具函数****************************
 ***********************************************************/

/** 手写的有界字符串拷贝（避免引入额外依赖，保证以 '\0' 结尾）。 */
static void __str_copy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;

    if (cap == 0) {
        return;
    }
    for (; i + 1 < cap && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/** 解析非负十进制整数（用于 elapsed 字段，当前布局不展示，仅为兼容协议）。 */
static uint32_t __atou(const char *s)
{
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

static ui_status_t __status_from_char(char c)
{
    switch (c) {
    case 'T': return ST_THINKING;
    case 'C': return ST_CODING;
    case 'V': return ST_TESTING;
    case 'E': return ST_ERROR;
    case 'I': return ST_IDLE;
    default:  return ST_THINKING;
    }
}

/** 由会话状态派生可视档位：出错->异常，空闲(完成)->已完成，其余->运行中。 */
static ui_vis_t __vis_from_status(ui_status_t s, bool done)
{
    if (s == ST_ERROR) {
        return VIS_ERROR;
    }
    if (done) {
        return VIS_DONE;
    }
    return VIS_RUNNING;
}

/** 按插件类型返回图标底色。 */
static lv_color_t __plugin_color(const char *plugin)
{
    if (strcmp(plugin, "claudecode") == 0) {
        return lv_color_hex(COL_ICON_CLAUDE);
    }
    if (strcmp(plugin, "codex") == 0) {
        return lv_color_hex(COL_ICON_CODEX);
    }
    if (strcmp(plugin, "opencode") == 0) {
        return lv_color_hex(COL_ICON_OPEN);
    }
    return lv_color_hex(COL_ICON_AGENT);
}

/***********************************************************
 *********************会话表增删改**************************
 ***********************************************************/

static ui_session_t *__find_session(const char *sid)
{
    uint16_t i;

    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used && strcmp(sg_sessions[i].sid, sid) == 0) {
            return &sg_sessions[i];
        }
    }
    return NULL;
}

static ui_session_t *__alloc_session(const char *sid)
{
    uint16_t i;

    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (!sg_sessions[i].used) {
            memset(&sg_sessions[i], 0, sizeof(sg_sessions[i]));
            sg_sessions[i].used = true;
            __str_copy(sg_sessions[i].sid, sid, sizeof(sg_sessions[i].sid));
            return &sg_sessions[i];
        }
    }
    return NULL;
}

/** 快照同步开始：把所有现有会话标记为“未见”，等待本轮 S 命令重新点亮。 */
static void __sync_begin(void)
{
    uint16_t i;

    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used) {
            sg_sessions[i].seen = false;
        }
    }
}

/** 快照同步结束：移除本轮未再出现（仍为“未见”）的会话，使列表与状态对齐。 */
static void __sync_end(void)
{
    uint16_t i;

    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used && !sg_sessions[i].seen) {
            sg_sessions[i].used        = false;
            sg_sessions[i].title_label = NULL;
            sg_structure_dirty         = true;
        }
    }
}

static void __upsert_session(const char *sid, ui_status_t st, uint32_t elapsed,
                             const char *plugin, const char *task)
{
    ui_session_t *s      = __find_session(sid);
    bool          is_new = false;
    ui_vis_t      new_vis;

    (void)elapsed; /* 当前布局不展示运行时长，仅保留协议兼容 */

    if (s == NULL) {
        s = __alloc_session(sid);
        if (s == NULL) {
            return; /* 会话表已满，丢弃 */
        }
        is_new = true;
    }

    __str_copy(s->plugin, (plugin != NULL && plugin[0] != '\0') ? plugin : "agent",
               sizeof(s->plugin));
    __str_copy(s->task, task != NULL ? task : "", sizeof(s->task));
    s->status = st;
    s->done   = (st == ST_IDLE);
    s->seen   = true; /* 本轮快照中出现过，E 时不会被清除 */

    new_vis = __vis_from_status(s->status, s->done);

    /* 新增会话或状态档位变化会改变行结构（增删行 / 切换指示灯类型），需重建列表；
     * 仅任务文本变化时就地更新标签即可，避免无谓重建。 */
    if (is_new || new_vis != s->vis) {
        sg_structure_dirty = true;
    } else if (s->title_label != NULL) {
        const char *body = s->task[0] != '\0' ? s->task
                                              : (s->plugin[0] != '\0' ? s->plugin : "—");
        lv_label_set_text(s->title_label, body);
    }
    s->vis = new_vis;
}

/***********************************************************
 ***********************命令行解析**************************
 ***********************************************************/

/**
 * @brief 解析一条命令行并更新会话表（会就地修改 line 缓冲区）。
 */
static void __ui_handle_line(char *line)
{
    char cmd = line[0];

    if (cmd == '\0') {
        return;
    }

    if (cmd == 'B' && line[1] == '\0') {
        __sync_begin();
        return;
    }

    if (cmd == 'E' && line[1] == '\0') {
        __sync_end();
        return;
    }

    if (cmd == 'G' && line[1] == '|') {
        /* 全局事件（泳道 2）：当前布局不展示，直接忽略。 */
        return;
    }

    if (cmd == 'S' && line[1] == '|') {
        /* S|sid|st|elapsed|plugin|task —— 去掉 "S|" 后按 4 个分隔符切出 5 段 */
        char *fields[5];
        char *cur = line + 2;
        int   nf  = 0;

        for (; nf < 4; nf++) {
            char *sep = strchr(cur, '|');
            if (sep == NULL) {
                break;
            }
            *sep = '\0';
            fields[nf] = cur;
            cur = sep + 1;
        }
        if (nf < 4) {
            return; /* 字段不足，视为非法命令 */
        }
        fields[4] = cur; /* 剩余部分即 task（可能为空） */

        __upsert_session(fields[0],
                         __status_from_char(fields[1][0]),
                         __atou(fields[2]),
                         fields[3],
                         fields[4]);
    }
}

/***********************************************************
 ***********************视图构建****************************
 ***********************************************************/

/** 创建一张“像素风”卡片：白底 + 近黑圆角描边 + 轻投影，不可滚动。 */
static lv_obj_t *__make_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);

    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_CARD_BORDER), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_radius(c, 14, 0);
    lv_obj_set_style_shadow_color(c, lv_color_hex(COL_CARD_BORDER), 0);
    lv_obj_set_style_shadow_width(c, 6, 0);
    lv_obj_set_style_shadow_ofs_y(c, 3, 0);
    lv_obj_set_style_shadow_opa(c, LV_OPA_20, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/** 创建一个透明无边框容器（用于内部布局分组，不可滚动）。 */
static lv_obj_t *__make_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);

    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/** 匀速旋转指示弧的动画回调：把 0~360 的值写入 arc 的整体旋转角。 */
static void __spinner_rotate_cb(void *var, int32_t v)
{
    lv_arc_set_rotation((lv_obj_t *)var, v);
}

/** 创建一个状态指示灯：运行中用匀速旋转弧，其余用对应颜色的圆点。 */
static lv_obj_t *__make_indicator(lv_obj_t *parent, ui_vis_t vis)
{
    if (vis == VIS_RUNNING) {
        /* 不用 lv_spinner：它内部同时跑“end 线性 + start 贝塞尔缓动”两条动画，
         * 弧长会“呼吸”、且循环收尾处速度突变，看起来像转一圈后闪回。
         * 这里改为“定长弧 + 匀速线性旋转”，360°≡0° 且速度恒定，循环点无缝衔接。 */
        lv_obj_t *arc = lv_arc_create(parent);
        lv_anim_t a;

        lv_obj_set_size(arc, 20, 20);
        lv_obj_set_style_pad_all(arc, 0, 0);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB); /* 去掉拖拽小圆点 */

        /* 背景轨道：淡色细环（降低存在感）。 */
        lv_arc_set_bg_angles(arc, 0, 360);
        lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_40, LV_PART_MAIN);

        /* 指示弧：定长 270°、略粗、圆角端帽。 */
        lv_arc_set_angles(arc, 0, 270);
        lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(COL_RUNNING), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

        /* 匀速线性旋转：单圈 1500ms，无限循环；线性路径保证收尾处速度不突变。 */
        lv_anim_init(&a);
        lv_anim_set_var(&a, arc);
        lv_anim_set_exec_cb(&a, __spinner_rotate_cb);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_duration(&a, 1500);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        return arc;
    } else {
        lv_obj_t *dot = lv_obj_create(parent);
        uint32_t  hex;

        switch (vis) {
        case VIS_DONE:  hex = COL_DONE;   break;
        case VIS_ERROR: hex = COL_ERROR;  break;
        default:        hex = COL_VIEWED; break;
        }
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(hex), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        return dot;
    }
}

/** 创建插件图标（彩色圆角方块 + 近黑描边），色彩由 plugin_type 决定。 */
static lv_obj_t *__make_icon(lv_obj_t *parent, const char *plugin)
{
    lv_obj_t *icon = lv_obj_create(parent);

    lv_obj_set_size(icon, 26, 26);
    lv_obj_set_style_radius(icon, 7, 0);
    lv_obj_set_style_bg_color(icon, __plugin_color(plugin), 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(icon, lv_color_hex(COL_CARD_BORDER), 0);
    lv_obj_set_style_border_width(icon, 2, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
    return icon;
}

/** 图例中的单个条目：指示灯 + 文字（透明容器，横向排列）。 */
static void __make_legend_item(lv_obj_t *parent, ui_vis_t vis, const char *text)
{
    lv_obj_t *item = __make_plain(parent);
    lv_obj_t *label;

    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 4, 0);

    __make_indicator(item, vis);

    label = lv_label_create(item);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TITLE), 0);
}

/** 构建底部图例卡片外框（内容由 __rebuild_legend 依据连接状态动态填充）。 */
static void __build_legend(lv_obj_t *parent)
{
    sg_legend = __make_card(parent);

    lv_obj_set_width(sg_legend, LV_PCT(100));
    lv_obj_set_height(sg_legend, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(sg_legend, 10, 0);
    lv_obj_set_style_pad_ver(sg_legend, 6, 0);
    lv_obj_set_flex_flow(sg_legend, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(sg_legend, 8, 0);
    lv_obj_set_style_pad_row(sg_legend, 4, 0);
}

/**
 * @brief 依据蓝牙连接状态刷新图例内容。
 *
 * 未连接时居中展示“请连接蓝牙”提示；已连接时展示状态图例（异常 / 已完成 / 已查看）。
 */
static void __rebuild_legend(void)
{
    if (sg_legend == NULL) {
        return;
    }
    lv_obj_clean(sg_legend);

    if (!openwaifu_ble_is_connected()) {
        lv_obj_t *tip = lv_label_create(sg_legend);

        lv_label_set_text(tip, "请连接蓝牙");
        lv_obj_set_style_text_color(tip, lv_color_hex(COL_TEXT_SUB), 0);
        lv_obj_set_flex_align(sg_legend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        return;
    }

    lv_obj_set_flex_align(sg_legend, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    __make_legend_item(sg_legend, VIS_ERROR, "异常");
    __make_legend_item(sg_legend, VIS_DONE, "已完成");
    __make_legend_item(sg_legend, VIS_VIEWED, "已查看");
}

/** 构建左栏虚拟形象卡片：内嵌帧序列动画（当前播放思考/加载序列）。 */
static void __build_avatar(lv_obj_t *parent)
{
    lv_obj_t *avatar = __make_card(parent);

    lv_obj_set_width(avatar, 188);
    lv_obj_set_height(avatar, LV_PCT(100));
    /* 虚拟形象动画：由 openwaifu_avatar 模块管理帧序列，后续可按状态切换序列。 */
    openwaifu_avatar_create(avatar);
}

/** 按当前会话表重建右栏任务清单（清空并按稳定顺序重新生成行）。 */
static void __rebuild_list(void)
{
    ui_session_t *order[OPENWAIFU_UI_MAX_SESSIONS];
    uint16_t      cnt = 0;
    uint16_t      i, j;

    lv_obj_clean(sg_list);
    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        sg_sessions[i].title_label = NULL;
    }

    /* 收集所有活跃会话 */
    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used) {
            order[cnt++] = &sg_sessions[i];
        }
    }

    /* 空状态：居中提示（依据连接状态给出有意义文案）。 */
    if (cnt == 0) {
        lv_obj_t *hint = lv_label_create(sg_list);

        lv_label_set_text(hint, "暂无任务");
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_SUB), 0);
        lv_obj_set_flex_align(sg_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        return;
    }
    lv_obj_set_flex_align(sg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    /* 按会话 ID（Topic）稳定排序（插入排序）：会话 ID 一旦分配便不再变化，
     * 据此排序可保证列表顺序固定，不会因会话增删或状态更新而反复跳动。 */
    for (i = 1; i < cnt; i++) {
        ui_session_t *key = order[i];
        j = i;
        while (j > 0 && strcmp(order[j - 1]->sid, key->sid) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }

    for (i = 0; i < cnt; i++) {
        ui_session_t *s = order[i];
        lv_obj_t     *row;
        const char   *body;

        row = __make_card(sg_list);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(row, 8, 0);
        lv_obj_set_style_pad_ver(row, 5, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);

        /* 插件图标（左，定宽彩色方块） */
        __make_icon(row, s->plugin);

        /* 任务主题（主体，占据剩余空间，超长省略）。任务为空时回退到插件名，
         * 避免主体长期只显示占位符。 */
        body = s->task[0] != '\0' ? s->task : (s->plugin[0] != '\0' ? s->plugin : "—");
        s->title_label = lv_label_create(row);
        lv_obj_set_flex_grow(s->title_label, 1);
        lv_label_set_long_mode(s->title_label, LV_LABEL_LONG_DOT);
        lv_label_set_text(s->title_label, body);
        lv_obj_set_style_text_color(s->title_label, lv_color_hex(COL_TITLE), 0);

        /* 状态指示灯（右，spinner 或彩色圆点） */
        __make_indicator(row, s->vis);
    }
}

/***********************************************************
 ***********************刷新回调****************************
 ***********************************************************/

/**
 * @brief LVGL 定时器回调：拉取 BLE 命令 -> 必要时重建任务清单。
 *
 * 运行在 LVGL 任务上下文中，可安全操作 LVGL 对象。运行中会话的 spinner
 * 由 LVGL 动画自动驱动，无需在此逐帧刷新。
 */
static void __ui_refresh_cb(lv_timer_t *timer)
{
    char line[OPENWAIFU_BLE_MAX_MSG_LEN + 1];
    bool conn;

    (void)timer;

    /* 逐条取出并解析 BLE 命令行 */
    while (openwaifu_ble_fetch_message(line, sizeof(line))) {
        __ui_handle_line(line);
    }

    /* 连接状态变化时刷新图例（未连接提示请连接，已连接展示状态图例）。 */
    conn = openwaifu_ble_is_connected() ? true : false;
    if (conn != sg_conn_last) {
        sg_conn_last = conn;
        __rebuild_legend();
    }

    if (sg_structure_dirty) {
        __rebuild_list();
        sg_structure_dirty = false;
    }
}

/***********************************************************
 ***********************对外接口****************************
 ***********************************************************/

void openwaifu_ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t     *screen;
    lv_obj_t     *root;
    lv_obj_t     *right;

    /* 将屏幕旋转为横屏（物理竖屏 320x480 -> 逻辑横屏 480x320），
     * 由 LVGL 端软件旋转完成，无需改动板级配置。 */
    if (disp != NULL) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    }

    screen = lv_screen_active();

    /* 浅色背景 + 统一文本样式（CJK 字体） */
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COL_TITLE), 0);
    lv_obj_set_style_text_font(screen, openwaifu_font(), 0);

    /* 根容器：左右两栏（左虚拟形象 + 右任务清单）。 */
    root = __make_plain(screen);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(root, 10, 0);
    lv_obj_set_style_pad_column(root, 10, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    /* 左栏：虚拟形象占位卡片。 */
    __build_avatar(root);

    /* 右栏：纵向排列的「任务清单 + 图例」。 */
    right = __make_plain(root);
    lv_obj_set_height(right, LV_PCT(100));
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(right, 8, 0);

    /* 任务清单容器：占据右栏剩余空间，超出时可纵向滚动。 */
    sg_list = __make_plain(right);
    lv_obj_set_width(sg_list, LV_PCT(100));
    lv_obj_set_flex_grow(sg_list, 1);
    lv_obj_set_flex_flow(sg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sg_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(sg_list, 6, 0);
    /* 为投影和滚动条留出一点右内边距，避免描边被裁切。 */
    lv_obj_set_style_pad_right(sg_list, 4, 0);

    /* 底部图例（固定于右栏底部，内容依连接状态动态刷新）。 */
    __build_legend(right);
    sg_conn_last = openwaifu_ble_is_connected() ? true : false;
    __rebuild_legend();

    /* 首帧构建列表（空状态）并启动周期刷新。 */
    sg_structure_dirty = true;
    __rebuild_list();
    sg_structure_dirty = false;
    lv_timer_create(__ui_refresh_cb, OPENWAIFU_UI_REFRESH_MS, NULL);
}
