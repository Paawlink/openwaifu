/**
 * @file openwaifu_ui.c
 * @brief OpenWaifu 会话看板 UI（LVGL 9）+ 情绪状态机。
 *
 * 界面实时展示守护进程（OpenWaifuD）推送的“活跃 Agent 会话”，每个会话固定占
 * 一行/一张卡片，而非滚动历史。根据当前活跃会话数量切换设备“情绪”档位：
 *
 *   0 个任务   -> 睡觉中     （居中大号 Zzz）
 *   1 个任务   -> 摸鱼中     （单任务大卡片：大号计时器）
 *   2-3 个任务 -> 认真搬砖   （会话列表）
 *   4-6 个任务 -> 火力全开   （会话列表）
 *   >6 个任务  -> 要炸了     （会话列表）
 *
 * 界面采用横屏布局（480x320）。列表中每个会话以“任务主题简介”为主体（占据行
 * 左侧大部分空间），状态（思考中/编码中/测试中/出错/完成）与已运行时长统一放在
 * 行右侧。为避免会话增删导致顺序反复跳动，列表按会话 ID（Topic）稳定排序。
 * 时长在本地按秒实时跳动，无需依赖推送频率。
 *
 * 数据来源：BLE 模块按 FIFO 暂存来自守护进程的命令行，UI 定时器逐条取出解析：
 *   C                                  清空所有会话
 *   X|<sid>                            移除某会话（完成/中止/超时）
 *   S|<sid>|<st>|<elapsed>|<plugin>|<task>  新增或更新某会话
 *   B                                  快照同步开始（把现有会话标记为“未见”）
 *   E                                  快照同步结束（移除本轮未再出现的会话）
 * 其中 <st> 为单字符状态码：T=思考 C=编码 V=测试 E=出错 I=空闲(完成)。
 * B/E 用于周期性快照对账，使屏幕列表无闪烁地收敛到与守护进程状态完全一致。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#include "openwaifu_ui.h"

#include <string.h>

#include "lvgl.h"
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
/** UI 刷新定时器周期（毫秒）：既用于拉取命令，也用于计时器跳秒。 */
#define OPENWAIFU_UI_REFRESH_MS   500

/* 配色（深色主题，突出状态色） */
#define COL_BG          0x0E1116
#define COL_CARD        0x1B2029
#define COL_CARD_BORDER 0x2C333F
#define COL_TEXT        0xF2F4F8
#define COL_TEXT_SUB    0x9AA4B2
#define COL_THINKING    0x4A9EFF
#define COL_CODING      0x35C759
#define COL_TESTING     0xFF9F0A
#define COL_ERROR       0xFF453A
#define COL_IDLE        0x8E8E93
#define COL_DONE        0x35C759
#define COL_MOOD_SINGLE 0x30D5C8

/***********************************************************
 ***********************类型定义****************************
 ***********************************************************/
/** 会话运行状态。 */
typedef enum {
    ST_THINKING,
    ST_CODING,
    ST_TESTING,
    ST_ERROR,
    ST_IDLE,
} ui_status_t;

/** 情绪档位（由活跃会话数量派生，决定整体布局）。 */
typedef enum {
    TIER_SLEEP,    /* 0 个任务 */
    TIER_SINGLE,   /* 1 个任务 */
    TIER_FEW,      /* 2-3 个任务 */
    TIER_MANY,     /* 4-6 个任务 */
    TIER_OVERLOAD, /* >6 个任务 */
} ui_tier_t;

/** 单个会话的本地状态与其绑定的 LVGL 标签。 */
typedef struct {
    bool        used;
    bool        seen;         /* 快照同步标记：B 置为 false，S 置为 true，E 时 false 则移除 */
    char        sid[OPENWAIFU_UI_SID_LEN];
    char        plugin[OPENWAIFU_UI_PLUGIN_LEN];
    char        task[OPENWAIFU_UI_TASK_LEN];
    ui_status_t status;
    bool        done;         /* 收到 idle/完成后置位，计时冻结并显示“完成” */
    uint32_t    base_elapsed; /* 守护进程下发的已运行秒数（基准） */
    uint32_t    recv_tick;    /* 收到该更新时的本地 tick，用于本地跳秒 */
    lv_obj_t   *status_label; /* 状态标签（随状态变色） */
    lv_obj_t   *task_label;   /* 任务文本标签 */
    lv_obj_t   *timer_label;  /* 已运行时长标签 */
} ui_session_t;

/***********************************************************
 ***********************变量定义****************************
 ***********************************************************/
static ui_session_t sg_sessions[OPENWAIFU_UI_MAX_SESSIONS];
static lv_obj_t    *sg_conn_label       = NULL; /* 顶部连接状态 */
static lv_obj_t    *sg_mood_label        = NULL; /* 情绪档位标题 */
static lv_obj_t    *sg_root              = NULL; /* 会话视图容器（按档位重建） */
static ui_tier_t    sg_tier              = TIER_SLEEP;
static bool         sg_structure_dirty   = true; /* 会话增删或档位变化时需整体重建 */

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

/** 解析非负十进制整数（用于 elapsed 字段）。 */
static uint32_t __atou(const char *s)
{
    uint32_t v = 0;

    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (uint32_t)(*s - '0');
        s++;
    }
    return v;
}

/** 大号字体：优先 Montserrat 40 -> 28 -> 24，最后回退到 CJK 字体。 */
static const lv_font_t *__font_big(void)
{
#if LV_FONT_MONTSERRAT_40
    return &lv_font_montserrat_40;
#elif LV_FONT_MONTSERRAT_28
    return &lv_font_montserrat_28;
#elif LV_FONT_MONTSERRAT_24
    return &lv_font_montserrat_24;
#else
    return openwaifu_font();
#endif
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

static const char *__status_text(ui_status_t s, bool done)
{
    if (done) {
        return "完成";
    }
    switch (s) {
    case ST_THINKING: return "思考中";
    case ST_CODING:   return "编码中";
    case ST_TESTING:  return "测试中";
    case ST_ERROR:    return "出错";
    default:          return "空闲";
    }
}

static lv_color_t __status_color(ui_status_t s, bool done)
{
    uint32_t hex;

    if (done) {
        hex = COL_DONE;
    } else {
        switch (s) {
        case ST_THINKING: hex = COL_THINKING; break;
        case ST_CODING:   hex = COL_CODING;   break;
        case ST_TESTING:  hex = COL_TESTING;  break;
        case ST_ERROR:    hex = COL_ERROR;    break;
        default:          hex = COL_IDLE;     break;
        }
    }
    return lv_color_hex(hex);
}

/** 将秒数格式化为 "MM:SS"（<1h）或 "H:MM:SS"。 */
static void __fmt_elapsed(uint32_t secs, char *buf, uint32_t n)
{
    uint32_t h = secs / 3600;
    uint32_t m = (secs % 3600) / 60;
    uint32_t s = secs % 60;

    if (h > 0) {
        lv_snprintf(buf, n, "%u:%02u:%02u", (unsigned)h, (unsigned)m, (unsigned)s);
    } else {
        lv_snprintf(buf, n, "%02u:%02u", (unsigned)m, (unsigned)s);
    }
}

/** 计算会话当前已运行秒数：完成后冻结，否则本地跳秒累加。 */
static uint32_t __session_elapsed(const ui_session_t *s)
{
    if (s->done) {
        return s->base_elapsed;
    }
    return s->base_elapsed + lv_tick_elaps(s->recv_tick) / 1000;
}

static ui_tier_t __tier_from_count(uint32_t n)
{
    if (n == 0) {
        return TIER_SLEEP;
    }
    if (n == 1) {
        return TIER_SINGLE;
    }
    if (n <= 3) {
        return TIER_FEW;
    }
    if (n <= 6) {
        return TIER_MANY;
    }
    return TIER_OVERLOAD;
}

/***********************************************************
 *********************会话表增删改**************************
 ***********************************************************/

static uint32_t __active_count(void)
{
    uint32_t n = 0;
    uint16_t i;

    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used) {
            n++;
        }
    }
    return n;
}

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

static void __clear_sessions(void)
{
    uint16_t i;

    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        sg_sessions[i].used         = false;
        sg_sessions[i].status_label = NULL;
        sg_sessions[i].task_label   = NULL;
        sg_sessions[i].timer_label  = NULL;
    }
    sg_structure_dirty = true;
}

static void __remove_session(const char *sid)
{
    ui_session_t *s = __find_session(sid);

    if (s != NULL) {
        s->used         = false;
        s->status_label = NULL;
        s->task_label   = NULL;
        s->timer_label  = NULL;
        sg_structure_dirty = true;
    }
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
            sg_sessions[i].used         = false;
            sg_sessions[i].status_label = NULL;
            sg_sessions[i].task_label   = NULL;
            sg_sessions[i].timer_label  = NULL;
            sg_structure_dirty          = true;
        }
    }
}

static void __upsert_session(const char *sid, ui_status_t st, uint32_t elapsed,
                             const char *plugin, const char *task)
{
    ui_session_t *s      = __find_session(sid);
    bool          is_new = false;

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
    s->status       = st;
    s->done         = (st == ST_IDLE);
    s->base_elapsed = elapsed;
    s->recv_tick    = lv_tick_get();
    s->seen         = true; /* 本轮快照中出现过，E 时不会被清除 */

    /* 仅“新增”会改变会话数量/档位，需要整体重建；纯状态更新走原地重绘。 */
    if (is_new) {
        sg_structure_dirty = true;
    }
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

    if (cmd == 'C' && line[1] == '\0') {
        __clear_sessions();
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

    if (cmd == 'X' && line[1] == '|') {
        __remove_session(line + 2);
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

/** 创建一张卡片容器（深色背景 + 圆角 + 边框，不可滚动）。 */
static lv_obj_t *__make_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);

    lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(COL_CARD_BORDER), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 12, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/** 创建一个透明的无边框容器（用于内部布局分组）。 */
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

/** 刷新单个会话绑定标签的文本与颜色。 */
static void __paint_session(ui_session_t *s)
{
    char       buf[16];
    lv_color_t col = __status_color(s->status, s->done);

    if (s->status_label != NULL) {
        lv_label_set_text(s->status_label, __status_text(s->status, s->done));
        lv_obj_set_style_text_color(s->status_label, col, 0);
    }
    if (s->task_label != NULL) {
        /* 行主体优先显示任务主题；任务文本为空时回退到插件（Topic）名，
         * 避免主体长期只显示占位符。 */
        const char *body = s->task[0] != '\0' ? s->task : (s->plugin[0] != '\0' ? s->plugin : "—");
        lv_label_set_text(s->task_label, body);
        /* 行主体（任务主题）固定用白色，保证在深色卡片上清晰可读。 */
        lv_obj_set_style_text_color(s->task_label, lv_color_hex(COL_TEXT), 0);
    }
    if (s->timer_label != NULL) {
        __fmt_elapsed(__session_elapsed(s), buf, sizeof(buf));
        lv_label_set_text(s->timer_label, buf);
        lv_obj_set_style_text_color(s->timer_label, col, 0);
    }
}

/** 刷新情绪标题（档位名 + 任务数）。 */
static void __paint_mood(ui_tier_t t, uint32_t n)
{
    const char *name;
    uint32_t    hex;

    switch (t) {
    case TIER_SLEEP:    name = "睡觉中";   hex = COL_IDLE;        break;
    case TIER_SINGLE:   name = "摸鱼中";   hex = COL_MOOD_SINGLE; break;
    case TIER_FEW:      name = "认真搬砖"; hex = COL_THINKING;    break;
    case TIER_MANY:     name = "火力全开"; hex = COL_TESTING;     break;
    default:            name = "要炸了";   hex = COL_ERROR;       break;
    }

    if (t == TIER_SLEEP) {
        lv_label_set_text(sg_mood_label, name);
    } else {
        lv_label_set_text_fmt(sg_mood_label, "%s · %u 个任务", name, (unsigned)n);
    }
    lv_obj_set_style_text_color(sg_mood_label, lv_color_hex(hex), 0);
}

/** 睡觉视图：居中大号 Zzz + 提示。 */
static void __build_sleep(void)
{
    lv_obj_t *box = __make_plain(sg_root);
    lv_obj_t *zzz;
    lv_obj_t *hint;

    lv_obj_set_size(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 12, 0);

    zzz = lv_label_create(box);
    lv_label_set_text(zzz, "Zzz");
    lv_obj_set_style_text_font(zzz, __font_big(), 0);
    lv_obj_set_style_text_color(zzz, lv_color_hex(COL_IDLE), 0);

    hint = lv_label_create(box);
    lv_label_set_text(hint, "没有任务，摸鱼待命中");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_SUB), 0);
}

/** 会话列表视图：每个会话固定占一行，任务主题为主体、状态与时长靠右。 */
static void __build_list(void)
{
    ui_session_t *order[OPENWAIFU_UI_MAX_SESSIONS];
    uint16_t      cnt = 0;
    uint16_t      i, j;

    /* 先收集所有活跃会话 */
    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used) {
            order[cnt++] = &sg_sessions[i];
        }
    }

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

        row = __make_card(sg_root);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);

        /* 任务主题（主体，占据剩余空间，超长省略） */
        s->task_label = lv_label_create(row);
        lv_obj_set_flex_grow(s->task_label, 1);
        lv_label_set_long_mode(s->task_label, LV_LABEL_LONG_DOT);

        /* 状态（右侧定宽，随状态变色，右对齐） */
        s->status_label = lv_label_create(row);
        lv_obj_set_width(s->status_label, 64);
        lv_obj_set_style_text_align(s->status_label, LV_TEXT_ALIGN_RIGHT, 0);

        /* 已运行时长（最右侧定宽，右对齐） */
        s->timer_label = lv_label_create(row);
        lv_obj_set_width(s->timer_label, 72);
        lv_obj_set_style_text_align(s->timer_label, LV_TEXT_ALIGN_RIGHT, 0);

        __paint_session(s);
    }
}

/** 按当前会话数量整体重建视图（清空并按档位重新布局）。 */
static void __rebuild(void)
{
    uint32_t n;
    uint16_t i;

    lv_obj_clean(sg_root);
    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        sg_sessions[i].status_label = NULL;
        sg_sessions[i].task_label   = NULL;
        sg_sessions[i].timer_label  = NULL;
    }

    n       = __active_count();
    sg_tier = __tier_from_count(n);
    __paint_mood(sg_tier, n);

    switch (sg_tier) {
    case TIER_SLEEP: __build_sleep(); break;
    default:         __build_list();  break;
    }
}

/***********************************************************
 ***********************刷新回调****************************
 ***********************************************************/

/**
 * @brief LVGL 定时器回调：拉取命令 -> 必要时重建 -> 刷新计时器。
 *
 * 运行在 LVGL 任务上下文中，可安全操作 LVGL 对象。
 */
static void __ui_refresh_cb(lv_timer_t *timer)
{
    char     line[OPENWAIFU_BLE_MAX_MSG_LEN + 1];
    uint16_t i;

    (void)timer;

    /* 逐条取出并解析 BLE 命令行 */
    while (openwaifu_ble_fetch_message(line, sizeof(line))) {
        __ui_handle_line(line);
    }

    /* 连接状态 */
    if (openwaifu_ble_is_connected()) {
        lv_label_set_text(sg_conn_label, "已连接");
        lv_obj_set_style_text_color(sg_conn_label, lv_color_hex(COL_CODING), 0);
    } else {
        lv_label_set_text(sg_conn_label, "未连接");
        lv_obj_set_style_text_color(sg_conn_label, lv_color_hex(COL_IDLE), 0);
    }

    if (sg_structure_dirty) {
        __rebuild();
        sg_structure_dirty = false;
    } else {
        /* 无结构变化：原地刷新状态/任务/计时器（含本地跳秒） */
        for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
            if (sg_sessions[i].used) {
                __paint_session(&sg_sessions[i]);
            }
        }
    }
}

/***********************************************************
 ***********************对外接口****************************
 ***********************************************************/

void openwaifu_ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_obj_t     *screen;
    lv_obj_t     *title;

    /* 将屏幕旋转为横屏（物理竖屏 320x480 -> 逻辑横屏 480x320），
     * 由 LVGL 端软件旋转完成，无需改动板级配置。 */
    if (disp != NULL) {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    }

    screen = lv_screen_active();

    /* 深色背景 + 统一文本样式（CJK 字体） */
    lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(screen, openwaifu_font(), 0);

    /* 顶部标题栏：左标题 + 右连接状态 */
    title = lv_label_create(screen);
    lv_label_set_text(title, "OpenWaifu");
    lv_obj_set_style_text_color(title, lv_color_hex(COL_TEXT_SUB), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

    sg_conn_label = lv_label_create(screen);
    lv_label_set_text(sg_conn_label, "未连接");
    lv_obj_set_style_text_color(sg_conn_label, lv_color_hex(COL_IDLE), 0);
    lv_obj_align(sg_conn_label, LV_ALIGN_TOP_RIGHT, -12, 8);

    /* 情绪标题 */
    sg_mood_label = lv_label_create(screen);
    lv_label_set_text(sg_mood_label, "睡觉中");
    lv_obj_set_style_text_color(sg_mood_label, lv_color_hex(COL_IDLE), 0);
    lv_obj_align(sg_mood_label, LV_ALIGN_TOP_MID, 0, 34);

    /* 会话视图容器（按档位重建，填满标题栏以下区域） */
    sg_root = lv_obj_create(screen);
    lv_obj_set_size(sg_root, LV_PCT(100), 252);
    lv_obj_align(sg_root, LV_ALIGN_TOP_MID, 0, 62);
    lv_obj_set_style_bg_opa(sg_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sg_root, 0, 0);
    lv_obj_set_style_pad_all(sg_root, 8, 0);
    lv_obj_set_style_pad_row(sg_root, 8, 0);
    lv_obj_set_flex_flow(sg_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sg_root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 首帧构建睡觉视图并启动周期刷新 */
    sg_structure_dirty = true;
    lv_timer_create(__ui_refresh_cb, OPENWAIFU_UI_REFRESH_MS, NULL);
}
