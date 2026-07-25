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
 *   G|<ev>|<detail>                    全局事件（驱动虚拟形象事件状态：E=Error/X=Confused/D=Done）
 * 其中 <st> 为单字符状态码：T=思考 C=编码 V=测试 E=出错 I=空闲(完成)。
 * <ev> 为单字符事件码：E=出错 X=取消 D=完成（驱动桌宠事件形象）。
 * B/E 用于周期性快照对账，使屏幕列表无闪烁地收敛到与守护进程状态完全一致。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#include "openwaifu_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "openwaifu_avatar.h"
#include "openwaifu_ble.h"
#include "openwaifu_font.h"

/* 右栏会话列表的插件图标资源（27x27 内嵌 PNG，见 src/assets/icon_*.c）。 */
LV_IMAGE_DECLARE(icon_claude);   /* claudecode */
LV_IMAGE_DECLARE(icon_openai);   /* codex */
LV_IMAGE_DECLARE(icon_opencode); /* opencode */
LV_IMAGE_DECLARE(icon_qoder);    /* qoder */
LV_IMAGE_DECLARE(icon_tools);    /* tools */

/***********************************************************
 *************************宏定义****************************
 ***********************************************************/
/** 最多同时展示的会话数（超过后新会话将被丢弃，不入表）。 */
#define OPENWAIFU_UI_MAX_SESSIONS 8
/** 会话 ID / 插件名 / 任务文本的本地缓存长度。 */
#define OPENWAIFU_UI_SID_LEN      36
#define OPENWAIFU_UI_PLUGIN_LEN   16
#define OPENWAIFU_UI_TASK_LEN     96
/** 详情数据缓存长度。 */
#define OPENWAIFU_UI_ERR_LEN      96
#define OPENWAIFU_UI_META_LEN     80
#define OPENWAIFU_UI_CHAT_LEN     80
#define OPENWAIFU_UI_CHAT_ROLE_LEN 16
#define OPENWAIFU_UI_MAX_META     8
#define OPENWAIFU_UI_MAX_CHAT    12
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

#define UNKNOWN_SESSION_TITLE  "Applying patches"
#define UNKNOWN_SESSION_PLUGIN "tools"

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

/** 元数据条目（"key: value" 文本）。 */
typedef struct {
    char text[OPENWAIFU_UI_META_LEN];
} ui_meta_entry_t;

/** 聊天消息（角色 + 内容摘要）。 */
typedef struct {
    char role[OPENWAIFU_UI_CHAT_ROLE_LEN];
    char content[OPENWAIFU_UI_CHAT_LEN];
} ui_chat_msg_t;

/** 单个会话的本地状态与其绑定的 LVGL 标签。 */
typedef struct {
    bool        used;
    bool        seen;  /* 快照同步标记：B 置为 false，S 置为 true，E 时 false 则移除 */
    char        sid[OPENWAIFU_UI_SID_LEN];
    char        plugin[OPENWAIFU_UI_PLUGIN_LEN];
    char        task[OPENWAIFU_UI_TASK_LEN];
    char        error_msg[OPENWAIFU_UI_ERR_LEN]; /* 错误信息（D 命令 kind=0） */
    ui_status_t status;
    bool        done;        /* 收到 idle/完成后置位 */
    ui_vis_t    vis;         /* 缓存的可视档位，用于判断是否需要重建行 */
    lv_obj_t   *row;         /* 行容器（reconcile 时复用，避免重建导致 indicator 闪烁） */
    lv_obj_t   *plugin_icon; /* 插件图标（plugin 变化时原地替换） */
    lv_obj_t   *title_label; /* 任务主题标签（可就地更新文本） */
    lv_obj_t   *indicator;   /* 状态指示灯（vis 变化时原地替换） */
    uint32_t    elapsed;     /* 运行时长（秒），详情页展示 */
    /* 详情数据（D 命令同步） */
    ui_meta_entry_t meta[OPENWAIFU_UI_MAX_META];
    uint8_t         meta_count;
    ui_chat_msg_t   chat[OPENWAIFU_UI_MAX_CHAT];
    uint8_t         chat_count;
} ui_session_t;

/***********************************************************
 ***********************变量定义****************************
 ***********************************************************/
static ui_session_t sg_sessions[OPENWAIFU_UI_MAX_SESSIONS];
static lv_obj_t    *sg_list            = NULL; /* 右栏任务清单容器（按会话增删重建） */
static lv_obj_t    *sg_legend          = NULL; /* 底部图例卡片（按连接状态刷新内容） */
static bool         sg_conn_last       = false; /* 上次已展示的蓝牙连接状态 */
static bool         sg_structure_dirty = true; /* 会话增删时需协调列表 */
static lv_obj_t    *sg_empty_hint      = NULL; /* 空状态提示标签（避免每次刷新重建） */

/* 详情页状态 */
static lv_obj_t    *sg_main_screen     = NULL; /* 主屏幕（任务清单），用于从详情页返回 */
static lv_obj_t    *sg_detail_screen   = NULL; /* 详情页屏幕（NULL 表示未打开） */
static lv_obj_t    *sg_detail_content  = NULL; /* 详情页可滚动内容区（NULL 表示需全量构建） */
static lv_obj_t    *sg_detail_body     = NULL; /* 详情页动态数据区（刷新时仅重建此区域） */
static lv_obj_t    *sg_detail_indicator = NULL; /* 基本信息状态灯（状态不变时保持动画连续） */
static lv_obj_t    *sg_detail_status_label = NULL;
static lv_obj_t    *sg_detail_task_label   = NULL;
static lv_obj_t    *sg_detail_elapsed_label = NULL;
static ui_vis_t     sg_detail_indicator_vis = VIS_RUNNING;
static char         sg_detail_sid[OPENWAIFU_UI_SID_LEN] = ""; /* 当前查看的会话 ID */
static bool         sg_detail_dirty    = false;  /* 详情页内容需要刷新 */
static uint32_t     sg_detail_elapsed_base = 0; /* 进入详情页时的远端时长快照 */
static uint32_t     sg_detail_enter_tick   = 0; /* 进入详情页的本地 tick */
static uint32_t     sg_detail_displayed_elapsed = (uint32_t)-1;

/* ── 虚拟形象状态机 ───────────────────────────────────── */

/**
 * 桌宠形象分为三层：
 * 1. 基础状态：由当前会话列表决定——无活跃任务时为 IDLE（Moyu / Sleep 随机），
 *    有活跃任务时为 WORKING（Thinking / Coding / Cycling 随机）。
 * 2. 事件状态：错误 / 用户取消 / 任务完成等事件触发，临时展示对应形象约 5 秒，
 *    然后自动回落到基础状态。
 * 3. 随机重摇：基础状态下每隔约 15 秒随机重新选择一个形象，避免长期固定不变，
 *    但不会切换太快以防止分散注意力。
 */
typedef enum {
    AVATAR_BASE_IDLE,    /* 无活跃任务 */
    AVATAR_BASE_WORKING, /* 有活跃任务 */
} avatar_base_t;

#define AVATAR_EVENT_MS   5000  /* 事件形象展示时长（毫秒） */
#define AVATAR_REROLL_MS  15000 /* 基础形象重摇间隔（毫秒） */

static avatar_base_t sg_avatar_base         = AVATAR_BASE_IDLE;
static bool          sg_avatar_event_active  = false;       /* 正在展示事件形象 */
static openwaifu_avatar_state_t sg_avatar_event_state;      /* 当前事件形象，用于事件优先级 */
static lv_timer_t   *sg_avatar_event_timer   = NULL;        /* 事件超时定时器 */
static lv_timer_t   *sg_avatar_reroll_timer  = NULL;        /* 随机重摇定时器 */

/***********************************************************
 ***********************工具函数****************************
 ***********************************************************/

/* 前向声明：avatar 状态机函数在刷新回调小节定义，此处先声明以供会话增删逻辑调用。 */
static void __avatar_trigger_event(openwaifu_avatar_state_t state);
static void __avatar_update_base(void);
/* 前向声明：指示灯构建函数在视图构建小节定义，此处先声明以供会话更新逻辑原地替换。 */
static lv_obj_t *__make_indicator(lv_obj_t *parent, ui_vis_t vis);
static lv_obj_t *__make_icon(lv_obj_t *parent, const char *plugin);

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

/** 按插件类型返回图标底色（仅用于无对应软件图标时的回退方块）。 */
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

/** 按插件类型返回对应真实软件图标；未知类型返回 NULL（回退到彩色方块）。 */
static const lv_image_dsc_t *__plugin_icon_src(const char *plugin)
{
    if (strcmp(plugin, "claudecode") == 0) {
        return &icon_claude;
    }
    if (strcmp(plugin, "codex") == 0) {
        return &icon_openai;
    }
    if (strcmp(plugin, "opencode") == 0) {
        return &icon_opencode;
    }
    if (strcmp(plugin, "qoder") == 0) {
        return &icon_qoder;
    }
    if (strcmp(plugin, "tools") == 0) {
        return &icon_tools;
    }
    return NULL;
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
            /* 重用槽位前清理可能残留的行对象（会话被移除后槽位被复用） */
            if (sg_sessions[i].row != NULL) {
                lv_obj_delete(sg_sessions[i].row);
                sg_sessions[i].row       = NULL;
                sg_sessions[i].plugin_icon = NULL;
                sg_sessions[i].title_label = NULL;
                sg_sessions[i].indicator   = NULL;
            }
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
            sg_sessions[i].used = false;
            /* 不清除 row/title_label/indicator 指针：reconcile 需根据 row!=NULL
             * 判断哪些行需要删除。删除后由 reconcile 统一置 NULL。 */
            sg_structure_dirty  = true;
        }
    }
}

static void __upsert_session(const char *sid, ui_status_t st, uint32_t elapsed,
                             const char *plugin, const char *task)
{
    ui_session_t *s      = __find_session(sid);
    bool          is_new = false;
    bool          plugin_changed;
    ui_vis_t      new_vis;
    char          old_body[OPENWAIFU_UI_TASK_LEN];
    bool          unknown_name = task == NULL || task[0] == '\0';
    const char   *new_plugin =
        unknown_name ? UNKNOWN_SESSION_PLUGIN
                     : ((plugin != NULL && plugin[0] != '\0') ? plugin : "agent");
    const char *new_task = unknown_name ? UNKNOWN_SESSION_TITLE : task;

    if (s == NULL) {
        s = __alloc_session(sid);
        if (s == NULL) {
            return; /* 会话表已满，丢弃 */
        }
        is_new = true;
    }

    s->elapsed = elapsed; /* 详情页展示运行时长 */

    /* 覆盖前先记下“当前已显示的主体文本”。不能用 lv_label_get_text 比对：
     * LONG_DOT 会把省略号就地写回标签缓冲区，取回的是带“…”的截断串，与完整
     * body 永远不等，导致每帧都误判为“已变化”而重设文本。 */
    if (!is_new) {
        const char *ob = s->task[0] != '\0' ? s->task
                                            : (s->plugin[0] != '\0' ? s->plugin : "—");
        __str_copy(old_body, ob, sizeof(old_body));
    } else {
        old_body[0] = '\0';
    }

    plugin_changed = strcmp(s->plugin, new_plugin) != 0;
    __str_copy(s->plugin, new_plugin, sizeof(s->plugin));
    __str_copy(s->task, new_task, sizeof(s->task));
    {
        s->status = st;
        s->done   = (st == ST_IDLE);
    }
    s->seen   = true; /* 本轮快照中出现过，E 时不会被清除 */

    /* 每轮 S 命令重置详情数据，随后到达的 D 命令会全量重建 */
    s->meta_count  = 0;
    s->chat_count  = 0;
    s->error_msg[0] = '\0';

    new_vis = __vis_from_status(s->status, s->done);

    /* 新增会话需协调列表以创建行；状态档位变化时若行已存在则原地替换指示灯，
     * 避免全量重建导致其他会话的 spinner 动画中断闪烁。仅任务文本变化时就地更新标签。 */
    {
        bool need_rebuild = false;

        if (is_new) {
            need_rebuild = true;
        } else if (new_vis != s->vis) {
            if (s->row != NULL) {
                /* 行已存在：原地替换指示灯，无需全量重建 */
                if (s->indicator != NULL) {
                    lv_obj_delete(s->indicator);
                }
                s->indicator = __make_indicator(s->row, new_vis);
            } else {
                /* 行尚未创建：需要全量重建 */
                need_rebuild = true;
            }
        }

        if (need_rebuild) {
            sg_structure_dirty = true;
        } else {
            if (plugin_changed && s->row != NULL) {
                if (s->plugin_icon != NULL) {
                    lv_obj_delete(s->plugin_icon);
                }
                s->plugin_icon = __make_icon(s->row, s->plugin);
                lv_obj_move_to_index(s->plugin_icon, 0);
            }

            if (s->title_label != NULL) {
                const char *body = s->task[0] != '\0' ? s->task
                                                      : (s->plugin[0] != '\0' ? s->plugin : "—");
                /* 后端每 2s 全量下发一帧，多数情况下文本并未变化。仅在与上次显示的
                 * 主体文本不同时才更新：lv_label_set_text 即便文本相同也会 invalidate +
                 * 触发布局重算，叠加本屏软件旋转（ROTATION_90）重绘，会表现为“整体每隔
                 * 几秒跳一下”。这样既消除周期性重绘，也不打断运行中指示弧的动画。 */
                if (strcmp(old_body, body) != 0) {
                    lv_label_set_text(s->title_label, body);
                }
            }
        }
    }
    s->vis = new_vis;

    /* S 命令更新了状态/时长/任务等基本信息，若详情页正在展示此会话则需刷新 */
    if (sg_detail_screen != NULL && strcmp(sg_detail_sid, s->sid) == 0) {
        sg_detail_dirty = true;
    }
}

/***********************************************************
 ***********************命令行解析**************************
 ***********************************************************/

/**
 * @brief 处理会话详情命令（D|sid|kind|seq|text）。
 *
 * kind=0: 错误信息；kind=1: 元数据条目；kind=2: 聊天消息。
 * seq 为 0-based 序号，固件端据此写入对应槽位。每个会话的详情数据在
 * 快照同步开始（B）时不会清空——只有 S 命令会重建会话，D 命令随后
 * 覆盖详情数据。详情数据的清空由 S 命令触发（新会话或重新 upsert 时
 * 重置 meta_count/chat_count）。
 */
static void __handle_detail(char *line)
{
    /* D|sid|kind|seq|text —— 去掉 "D|" 后按 3 个分隔符切出 4 段 */
    char *fields[4];
    char *cur = line + 2;
    int   nf  = 0;
    uint32_t kind, seq;
    ui_session_t *s;

    for (; nf < 3; nf++) {
        char *sep = strchr(cur, '|');
        if (sep == NULL) {
            break;
        }
        *sep = '\0';
        fields[nf] = cur;
        cur = sep + 1;
    }
    if (nf < 3) {
        return;
    }
    fields[3] = cur; /* 剩余部分即 text */

    s = __find_session(fields[0]);
    if (s == NULL) {
        return; /* 会话不存在，丢弃详情数据 */
    }

    kind = __atou(fields[1]);
    seq  = __atou(fields[2]);

    if (kind == 0) {
        /* 错误信息 */
        __str_copy(s->error_msg, fields[3], sizeof(s->error_msg));
    } else if (kind == 1) {
        /* 元数据条目 */
        if (seq < OPENWAIFU_UI_MAX_META) {
            __str_copy(s->meta[seq].text, fields[3], sizeof(s->meta[0].text));
            if (seq + 1 > s->meta_count) {
                s->meta_count = (uint8_t)(seq + 1);
            }
        }
    } else if (kind == 2) {
        /* 聊天消息：text 格式为 "role: content" */
        if (seq < OPENWAIFU_UI_MAX_CHAT) {
            char *colon = strchr(fields[3], ':');
            if (colon != NULL) {
                *colon = '\0';
                __str_copy(s->chat[seq].role, fields[3], sizeof(s->chat[0].role));
                /* 跳过 ": " */
                char *content = colon + 1;
                if (*content == ' ') {
                    content++;
                }
                __str_copy(s->chat[seq].content, content, sizeof(s->chat[0].content));
            } else {
                __str_copy(s->chat[seq].role, "msg", sizeof(s->chat[0].role));
                __str_copy(s->chat[seq].content, fields[3], sizeof(s->chat[0].content));
            }
            if (seq + 1 > s->chat_count) {
                s->chat_count = (uint8_t)(seq + 1);
            }
        }
    }

    /* 如果正在查看此会话的详情页，标记需要刷新 */
    if (sg_detail_screen != NULL && strcmp(sg_detail_sid, fields[0]) == 0) {
        sg_detail_dirty = true;
    }
}

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
        /* 全局事件（泳道 2）：驱动虚拟形象事件状态。 */
        char ev = line[2];
        if (ev == 'E') {
            /* 错误事件 -> Error 形象 */
            __avatar_trigger_event(OPENWAIFU_AVATAR_ERROR);
        } else if (ev == 'X') {
            /* 用户取消事件 -> Confused 形象 */
            __avatar_trigger_event(OPENWAIFU_AVATAR_CONFUSED);
        } else if (ev == 'D') {
            /* 任务完成事件 -> Celebration 形象 */
            __avatar_trigger_event(OPENWAIFU_AVATAR_CELEBRATION);
        }
        return;
    }

    if (cmd == 'D' && line[1] == '|') {
        __handle_detail(line);
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

/* 前向声明：行点击回调在详情页小节定义，此处先声明以供 __rebuild_list 使用。 */
static void __row_click_cb(lv_event_t *e);
/* 前向声明：详情页构建函数在行点击回调之后定义。 */
static void __build_detail_view(void);

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
        lv_obj_t *arc = lv_arc_create(parent);
        lv_anim_t a;

        lv_obj_set_size(arc, 20, 20);
        lv_obj_set_style_pad_all(arc, 0, 0);
        lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_style(arc, NULL, LV_PART_KNOB);

        lv_arc_set_bg_angles(arc, 0, 360);
        lv_obj_set_style_arc_width(arc, 2, LV_PART_MAIN);
        lv_obj_set_style_arc_color(arc, lv_color_hex(COL_TRACK), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(arc, LV_OPA_40, LV_PART_MAIN);

        lv_arc_set_angles(arc, 0, 270);
        lv_obj_set_style_arc_width(arc, 3, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(arc, lv_color_hex(COL_RUNNING), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);

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

/** 创建插件图标：已知插件用对应真实软件图标（27x27 PNG），
 *  未知插件回退到彩色圆角方块 + 近黑描边（色彩由 plugin_type 决定）。 */
static lv_obj_t *__make_icon(lv_obj_t *parent, const char *plugin)
{
    const lv_image_dsc_t *src = __plugin_icon_src(plugin);

    if (src != NULL) {
        lv_obj_t *img = lv_image_create(parent);

        lv_image_set_src(img, src);
        lv_obj_set_size(img, 27, 27);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
        return img;
    }

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

/** 按当前会话表协调右栏任务清单（增量删除/创建/排序行，保留已有行的 indicator 动画）。 */
static void __rebuild_list(void)
{
    ui_session_t *order[OPENWAIFU_UI_MAX_SESSIONS];
    uint16_t      cnt = 0;
    uint16_t      i, j;

    /* Step 1: 删除已不再活跃的会话行（used=false 但 row 仍存在）。
     * 不清除 used=true 会话的行——它们的 indicator 动画需要保持连续。 */
    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (!sg_sessions[i].used && sg_sessions[i].row != NULL) {
            lv_obj_delete(sg_sessions[i].row);
            sg_sessions[i].row         = NULL;
            sg_sessions[i].plugin_icon = NULL;
            sg_sessions[i].title_label = NULL;
            sg_sessions[i].indicator   = NULL;
        }
    }

    /* 收集所有活跃会话 */
    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used) {
            order[cnt++] = &sg_sessions[i];
        }
    }

    /* 空状态：仅在首次进入时创建提示标签，避免每次刷新重建 */
    if (cnt == 0) {
        if (sg_empty_hint == NULL) {
            sg_empty_hint = lv_label_create(sg_list);
            lv_label_set_text(sg_empty_hint, "暂无任务");
            lv_obj_set_style_text_color(sg_empty_hint, lv_color_hex(COL_TEXT_SUB), 0);
        }
        lv_obj_set_flex_align(sg_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        return;
    }
    /* 非空状态：清除空状态提示（若存在） */
    if (sg_empty_hint != NULL) {
        lv_obj_delete(sg_empty_hint);
        sg_empty_hint = NULL;
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

    /* 为每个会话创建新行或调整已有行顺序。
     * 新行创建在 sg_list 末尾，随后通过 lv_obj_move_to_index 移到正确位置。
     * 已有行直接移动到目标索引——LVGL 会自动调整其他子对象的位置。
     * 这样已有行的 indicator（含 spinner 动画）不会被销毁重建，消除闪烁。 */
    for (i = 0; i < cnt; i++) {
        ui_session_t *s = order[i];

        if (s->row == NULL) {
            /* 新会话：创建完整行 */
            lv_obj_t *row = __make_card(sg_list);
            const char *body;

            lv_obj_set_width(row, LV_PCT(100));
            lv_obj_set_height(row, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_hor(row, 8, 0);
            lv_obj_set_style_pad_ver(row, 5, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(row, 8, 0);

            /* 使行可点击：点击后打开该会话的详情页 */
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, __row_click_cb, LV_EVENT_CLICKED, s);

            /* 插件图标（左，定宽彩色方块） */
            s->plugin_icon = __make_icon(row, s->plugin);

            /* 任务主题（主体，占据剩余空间，超长省略）。任务为空时回退到插件名，
             * 避免主体长期只显示占位符。 */
            body = s->task[0] != '\0' ? s->task : (s->plugin[0] != '\0' ? s->plugin : "—");
            s->title_label = lv_label_create(row);
            lv_obj_set_flex_grow(s->title_label, 1);
            lv_label_set_long_mode(s->title_label, LV_LABEL_LONG_DOT);
            lv_label_set_text(s->title_label, body);
            lv_obj_set_style_text_color(s->title_label, lv_color_hex(COL_TITLE), 0);

            /* 状态指示灯（右，spinner 或彩色圆点） */
            s->indicator = __make_indicator(row, s->vis);
            s->row       = row;

            /* 新行创建在末尾，移到正确位置 */
            lv_obj_move_to_index(row, (int32_t)i);
        } else {
            /* 已有行：移到正确位置（会话增删后顺序可能变化） */
            lv_obj_move_to_index(s->row, (int32_t)i);
        }
    }
}

/***********************************************************
 ***********************详情页******************************
 ***********************************************************/

/** 返回状态对应的中文标签。 */
static const char *__status_label(ui_status_t s)
{
    switch (s) {
    case ST_THINKING: return "思考中";
    case ST_CODING:   return "编码中";
    case ST_TESTING:  return "测试中";
    case ST_ERROR:    return "出错";
    case ST_IDLE:     return "已完成";
    default:          return "未知";
    }
}

/** 格式化运行时长（秒）为可读字符串。 */
static void __format_elapsed(uint32_t seconds, char *out, uint32_t out_size)
{
    if (seconds < 60) {
        snprintf(out, out_size, "%lus", (unsigned long)seconds);
    } else {
        snprintf(out, out_size, "%lum %lus", (unsigned long)(seconds / 60),
                 (unsigned long)(seconds % 60));
    }
}

/** 返回按钮点击回调：关闭详情页，返回主屏幕。 */
static void __detail_back_cb(lv_event_t *e)
{
    (void)e;
    if (sg_detail_screen != NULL) {
        lv_obj_delete(sg_detail_screen);
        sg_detail_screen  = NULL;
        sg_detail_content = NULL;
        sg_detail_body    = NULL;
        sg_detail_indicator = NULL;
        sg_detail_status_label = NULL;
        sg_detail_task_label = NULL;
        sg_detail_elapsed_label = NULL;
        sg_detail_displayed_elapsed = (uint32_t)-1;
        sg_detail_sid[0]  = '\0';
        sg_detail_dirty   = false;
        if (sg_main_screen != NULL) {
            lv_screen_load(sg_main_screen);
        }
    }
}

/** 行点击回调：打开对应会话的详情页。 */
static void __row_click_cb(lv_event_t *e)
{
    ui_session_t *s = (ui_session_t *)lv_event_get_user_data(e);

    if (s == NULL || !s->used) {
        return;
    }

    __str_copy(sg_detail_sid, s->sid, sizeof(sg_detail_sid));
    sg_detail_elapsed_base = s->elapsed;
    sg_detail_enter_tick   = lv_tick_get();
    sg_detail_displayed_elapsed = (uint32_t)-1;
    sg_detail_dirty   = true;
    sg_detail_content = NULL; /* 新详情页：需全量构建 */
    sg_detail_body    = NULL;

    if (sg_detail_screen != NULL) {
        lv_obj_delete(sg_detail_screen);
    }
    sg_detail_screen = lv_obj_create(NULL);
    lv_screen_load(sg_detail_screen);
    /* 立即构建详情内容，避免短暂空白 */
    __build_detail_view();
    sg_detail_dirty = false;
}

/** 在详情页内容区添加一行文本（带标签前缀）。 */
static lv_obj_t *__detail_add_label(lv_obj_t *parent, const char *prefix,
                                    const char *body, lv_color_t color)
{
    lv_obj_t *row = __make_plain(parent);
    lv_obj_t *lbl;
    char buf[OPENWAIFU_UI_TASK_LEN + 32];

    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);

    if (prefix != NULL && prefix[0] != '\0') {
        lbl = lv_label_create(row);
        lv_label_set_text(lbl, prefix);
        lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_SUB), 0);
    }

    lbl = lv_label_create(row);
    snprintf(buf, sizeof(buf), "%s", body != NULL ? body : "");
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(lbl, 1);
    return lbl;
}

/** 添加小节标题（如“元数据”“聊天上下文”）。 */
static void __detail_add_section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT_SUB), 0);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_obj_set_style_pad_top(lbl, 4, 0);
}

/** 构建详情页内容（在 sg_detail_screen 上填充）。 */
static void __build_detail_view(void)
{
    ui_session_t *s;
    lv_obj_t     *screen, *content;
    lv_obj_t     *card;
    char          buf[128];
    uint8_t       i;
    int32_t       scroll_y = 0; /* 刷新动态数据时保存/恢复滚动位置 */
    uint32_t      displayed_elapsed;

    if (sg_detail_screen == NULL) {
        return;
    }

    s = __find_session(sg_detail_sid);
    if (s == NULL || !s->used) {
        /* 会话已不存在，自动返回 */
        __detail_back_cb(NULL);
        return;
    }

    screen = sg_detail_screen;

    if (sg_detail_content == NULL) {
        /* 首次构建：设置屏幕样式 + 创建 header + 创建内容区 */
        lv_obj_t *header, *back, *title_lbl;

        lv_obj_set_style_bg_color(screen, lv_color_hex(COL_BG), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(screen, lv_color_hex(COL_TITLE), 0);
        lv_obj_set_style_text_font(screen, openwaifu_font(), 0);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clean(screen);
        lv_obj_set_style_pad_all(screen, 0, 0);
        lv_obj_set_style_pad_row(screen, 2, 0);
        lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);

        /* 顶部标题栏（固定，不滚动） */
        header = __make_card(screen);
        lv_obj_set_width(header, LV_PCT(100));
        lv_obj_set_height(header, 38);
        lv_obj_set_style_pad_hor(header, 12, 0);
        lv_obj_set_style_pad_ver(header, 4, 0);
        lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(header, 12, 0);
        lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
        back = lv_label_create(header);
        lv_label_set_text(back, "\xE2\x86\x90 \xE8\xBF\x94\xE5\x9B\x9E"); /* ← 返回 */
        lv_obj_set_style_text_color(back, lv_color_hex(COL_TITLE), 0);
        lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(back, __detail_back_cb, LV_EVENT_CLICKED, NULL);

        title_lbl = lv_label_create(header);
        lv_label_set_text(title_lbl, "\xE4\xBB\xBB\xE5\x8A\xA1\xE8\xAF\xA6\xE6\x83\x85"); /* 任务详情 */
        lv_obj_set_style_text_color(title_lbl, lv_color_hex(COL_TITLE), 0);

        /* 可滚动内容区 */
        content = lv_obj_create(screen);
        lv_obj_set_width(content, LV_PCT(100));
        lv_obj_set_flex_grow(content, 1);
        lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(content, 0, 0);
        lv_obj_set_style_pad_all(content, 10, 0);
        lv_obj_set_style_pad_bottom(content, 14, 0);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(content, 4, 0);
        sg_detail_content = content;

        /* —— 基本信息卡片 —— */
        card = __make_card(content);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(card, 10, 0);
        lv_obj_set_style_pad_ver(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 3, 0);

        /* 状态行：● 状态标签 · 插件 */
        lv_obj_t *stat_row = __make_plain(card);
        lv_obj_set_width(stat_row, LV_PCT(100));
        lv_obj_set_height(stat_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(stat_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stat_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(stat_row, 6, 0);

        sg_detail_indicator = __make_indicator(stat_row, s->vis);
        sg_detail_indicator_vis = s->vis;

        sg_detail_status_label = lv_label_create(stat_row);
        lv_obj_set_style_text_color(sg_detail_status_label, lv_color_hex(COL_TITLE), 0);

        /* 任务 */
        sg_detail_task_label = __detail_add_label(
            card, "\xE4\xBB\xBB\xE5\x8A\xA1:",
            s->task[0] != '\0' ? s->task : "\xE2\x80\x94", lv_color_hex(COL_TITLE));

        /* Session ID */
        __detail_add_label(card, "ID:", s->sid, lv_color_hex(COL_TEXT_SUB));

        /* 运行时长 */
        sg_detail_elapsed_label = __detail_add_label(
            card, "\xE6\x97\xB6\xE9\x95\xBF:", "", lv_color_hex(COL_TEXT_SUB));

        sg_detail_body = __make_plain(content);
        lv_obj_set_width(sg_detail_body, LV_PCT(100));
        lv_obj_set_height(sg_detail_body, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(sg_detail_body, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(sg_detail_body, 4, 0);
    } else {
        content = sg_detail_content;
    }

    displayed_elapsed = sg_detail_elapsed_base + lv_tick_elaps(sg_detail_enter_tick) / 1000;
    if (displayed_elapsed != sg_detail_displayed_elapsed) {
        __format_elapsed(displayed_elapsed, buf, sizeof(buf));
        lv_label_set_text(sg_detail_elapsed_label, buf);
        sg_detail_displayed_elapsed = displayed_elapsed;
    }

    if (sg_detail_dirty) {
        snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s", __status_label(s->status),
                 s->plugin[0] != '\0' ? s->plugin : "agent");
        lv_label_set_text(sg_detail_status_label, buf);
        lv_label_set_text(sg_detail_task_label,
                          s->task[0] != '\0' ? s->task : "\xE2\x80\x94");
    }

    if (sg_detail_indicator_vis != s->vis) {
        lv_obj_t *stat_row = lv_obj_get_parent(sg_detail_indicator);
        lv_obj_delete(sg_detail_indicator);
        sg_detail_indicator = __make_indicator(stat_row, s->vis);
        lv_obj_move_to_index(sg_detail_indicator, 0);
        sg_detail_indicator_vis = s->vis;
    }

    if (!sg_detail_dirty) {
        return;
    }

    scroll_y = lv_obj_get_scroll_y(content);
    lv_obj_clean(sg_detail_body);

    /* 错误信息（如果有） */
    if (s->error_msg[0] != '\0') {
        card = __make_card(sg_detail_body);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card, 8, 0);
        __detail_add_label(card, "\xE9\x94\x99\xE8\xAF\xAF:",
                           s->error_msg, lv_color_hex(COL_ERROR));
    }

    /* —— 元数据 —— */
    if (s->meta_count > 0) {
        __detail_add_section(sg_detail_body,
                             "\xE2\x80\x94\xE2\x80\x94 \xE5\x85\x83\xE6\x95\xB0\xE6\x8D\xAE \xE2\x80\x94\xE2\x80\x94");
        card = __make_card(sg_detail_body);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(card, 10, 0);
        lv_obj_set_style_pad_ver(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 2, 0);
        for (i = 0; i < s->meta_count; i++) {
            __detail_add_label(card, NULL, s->meta[i].text,
                               lv_color_hex(COL_TITLE));
        }
    }

    /* —— 聊天上下文 —— */
    if (s->chat_count > 0) {
        __detail_add_section(sg_detail_body,
                             "\xE2\x80\x94\xE2\x80\x94 \xE8\x81\x8A\xE5\xA4\xA9\xE4\xB8\x8A\xE4\xB8\x8B\xE6\x96\x87 \xE2\x80\x94\xE2\x80\x94");
        card = __make_card(sg_detail_body);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(card, 10, 0);
        lv_obj_set_style_pad_ver(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 3, 0);
        for (i = 0; i < s->chat_count; i++) {
            snprintf(buf, sizeof(buf), "[%s] %s",
                     s->chat[i].role, s->chat[i].content);
            __detail_add_label(card, NULL, buf, lv_color_hex(COL_TITLE));
        }
    }

    /* 空状态提示 */
    if (s->meta_count == 0 && s->chat_count == 0 && s->error_msg[0] == '\0') {
        lv_obj_t *hint = lv_label_create(sg_detail_body);
        lv_label_set_text(hint, "\xE6\x9A\x82\xE6\x97\xA0\xE8\xAF\xA6\xE6\x83\x85\xE6\x95\xB0\xE6\x8D\xAE");
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_SUB), 0);
    }

    /* 恢复刷新前的滚动位置（避免内容重建后跳回顶部） */
    if (scroll_y > 0) {
        lv_obj_update_layout(content);
        lv_obj_scroll_to_y(content, scroll_y, LV_ANIM_OFF);
    }
}

/***********************************************************
 *********************虚拟形象状态机************************
 ***********************************************************/

/** 随机选择一个空闲形象：Moyu 或 Sleep。 */
static openwaifu_avatar_state_t __avatar_pick_idle(void)
{
    return (rand() % 2 == 0) ? OPENWAIFU_AVATAR_MOYU : OPENWAIFU_AVATAR_SLEEP;
}

/** 随机选择一个工作形象：Thinking、Coding 或 Cycling。 */
static openwaifu_avatar_state_t __avatar_pick_working(void)
{
    switch (rand() % 3) {
    case 0:  return OPENWAIFU_AVATAR_THINKING;
    case 1:  return OPENWAIFU_AVATAR_CODING;
    default: return OPENWAIFU_AVATAR_CYCLING;
    }
}

/** 根据当前基础状态随机选择形象。 */
static openwaifu_avatar_state_t __avatar_pick_base(void)
{
    return (sg_avatar_base == AVATAR_BASE_IDLE) ? __avatar_pick_idle()
                                                 : __avatar_pick_working();
}

/** 事件超时回调：结束事件形象，回落到基础状态。 */
static void __avatar_event_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    sg_avatar_event_active = false;
    if (sg_avatar_event_timer != NULL) {
        lv_timer_delete(sg_avatar_event_timer);
        sg_avatar_event_timer = NULL;
    }
    /* 回落到基础状态形象 */
    openwaifu_avatar_set_state(__avatar_pick_base());
}

/** 触发一个事件形象（Error / Confused / Celebration），展示约 5 秒后自动回落。 */
static void __avatar_trigger_event(openwaifu_avatar_state_t state)
{
    /* 失败/取消事件展示期间忽略迟到的完成事件，避免 Celebration 覆盖真实结果。 */
    if (state == OPENWAIFU_AVATAR_CELEBRATION && sg_avatar_event_active &&
        (sg_avatar_event_state == OPENWAIFU_AVATAR_ERROR ||
         sg_avatar_event_state == OPENWAIFU_AVATAR_CONFUSED)) {
        return;
    }

    sg_avatar_event_active = true;
    sg_avatar_event_state  = state;
    openwaifu_avatar_set_state(state);

    /* （重新）启动事件超时定时器 */
    if (sg_avatar_event_timer != NULL) {
        lv_timer_delete(sg_avatar_event_timer);
    }
    sg_avatar_event_timer = lv_timer_create(__avatar_event_timer_cb,
                                            AVATAR_EVENT_MS, NULL);
    lv_timer_set_repeat_count(sg_avatar_event_timer, 1);
}

/** 随机重摇回调：每隔一段时间在当前基础状态的候选形象中重新选择一个。 */
static void __avatar_reroll_cb(lv_timer_t *timer)
{
    (void)timer;
    /* 事件展示中不重摇，等事件结束后自然会回落 */
    if (!sg_avatar_event_active) {
        openwaifu_avatar_state_t pick = __avatar_pick_base();
        openwaifu_avatar_set_state(pick);
    }
}

/**
 * 根据当前会话表重新计算基础状态（IDLE / WORKING）。
 * 有任意活跃（非完成）会话则为 WORKING，否则为 IDLE。
 * 基础状态变化时立即切换形象（事件展示中仅更新内部状态，不立即切换）。
 */
static void __avatar_update_base(void)
{
    uint16_t       i;
    bool           has_active = false;
    avatar_base_t  new_base;

    for (i = 0; i < OPENWAIFU_UI_MAX_SESSIONS; i++) {
        if (sg_sessions[i].used && !sg_sessions[i].done) {
            has_active = true;
            break;
        }
    }

    new_base = has_active ? AVATAR_BASE_WORKING : AVATAR_BASE_IDLE;
    if (new_base != sg_avatar_base) {
        sg_avatar_base = new_base;
        /* 基础状态变化时立即应用新形象（事件展示中则等结束后自动回落） */
        if (!sg_avatar_event_active) {
            openwaifu_avatar_set_state(__avatar_pick_base());
        }
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

    /* 详情页每轮仅检查本地秒数；数据变化时才重建动态详情区。 */
    if (sg_detail_screen != NULL) {
        __build_detail_view();
        sg_detail_dirty = false;
    }

    /* 更新虚拟形象基础状态（IDLE / WORKING） */
    __avatar_update_base();
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
    sg_main_screen = screen; /* 保存主屏幕引用，详情页返回时切回 */

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

    /* 任务清单容器：占据右栏剩余空间，超出时可纵向滚动。
     * __make_plain 默认清除了 SCROLLABLE 标志，这里需重新开启。 */
    sg_list = __make_plain(right);
    lv_obj_add_flag(sg_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(sg_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(sg_list, LV_SCROLLBAR_MODE_AUTO);
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

    /* 初始化虚拟形象状态机：随机种子 + 重摇定时器 + 初始形象 */
    srand((unsigned int)lv_tick_get());
    sg_avatar_base         = AVATAR_BASE_IDLE;
    sg_avatar_event_active  = false;
    sg_avatar_event_timer   = NULL;
    sg_avatar_reroll_timer  = lv_timer_create(__avatar_reroll_cb,
                                               AVATAR_REROLL_MS, NULL);
    openwaifu_avatar_set_state(__avatar_pick_idle());

    lv_timer_create(__ui_refresh_cb, OPENWAIFU_UI_REFRESH_MS, NULL);
}
