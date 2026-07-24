/**
 * @file openwaifu_ble.c
 * @brief OpenWaifu BLE 从机（Peripheral）实现。
 *
 * 设备以名称 "OpenWaifu" 对外广播，接受电脑端连接，并将写入到标准 GATT
 * Write 特征的 UTF-8 文本消息暂存下来，供 UI 模块显示。消息在存入前会做
 * UTF-8 合法性校验，非法数据仅打印日志、不入队，避免污染 LVGL 文本渲染。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#include "openwaifu_ble.h"

#include "tal_api.h"
#include "tal_bluetooth.h"

/***********************************************************
 *************************宏定义****************************
 ***********************************************************/
/** 广播中使用的 16-bit Service UUID（0xFD50 为涂鸦标准 GATT 服务）。 */
#define OPENWAIFU_BLE_ADV_SVC_UUID 0xFD50

/** 接收命令行 FIFO 的容量（一次 resync 可能连续下发“清空 + 多条会话”）。 */
#define OPENWAIFU_BLE_QUEUE_LEN    16

/***********************************************************
 ***********************变量定义****************************
 ***********************************************************/
static MUTEX_HANDLE sg_msg_mutex     = NULL;  /* 保护下方共享消息状态 */
static bool         sg_ble_connected = false; /* 当前 BLE 连接状态 */

/* 命令行环形缓冲：BLE 任务写入尾部，UI 任务从头部取出（先进先出）。 */
static char     sg_ring[OPENWAIFU_BLE_QUEUE_LEN][OPENWAIFU_BLE_MAX_MSG_LEN + 1];
static uint16_t sg_ring_head  = 0; /* 下一个可读取的位置 */
static uint16_t sg_ring_tail  = 0; /* 下一个可写入的位置 */
static uint16_t sg_ring_count = 0; /* 当前缓存的命令行条数 */

/**
 * @brief 广播数据（AD 结构）。
 * - 0x02 0x01 0x06：Flags（LE General Discoverable + BR/EDR Not Supported）
 * - 0x03 0x03 XX XX：完整 16-bit Service UUID 列表（小端）
 */
static uint8_t sg_adv_data[] = {
    0x02, 0x01, 0x06,
    0x03, 0x03, (uint8_t)(OPENWAIFU_BLE_ADV_SVC_UUID & 0xFF),
                (uint8_t)(OPENWAIFU_BLE_ADV_SVC_UUID >> 8),
};

/**
 * @brief 扫描响应数据，携带设备名 "OpenWaifu"（0x09 为 Complete Local Name）。
 */
static uint8_t sg_scan_rsp_data[] = {
    0x0A, 0x09, 'O', 'p', 'e', 'n', 'W', 'a', 'i', 'f', 'u',
};

/***********************************************************
 ***********************函数定义****************************
 ***********************************************************/

/**
 * @brief 校验字节序列是否为合法的 UTF-8 编码。
 *
 * 截断的多字节序列、以及 overlong / 代理区（surrogate）等非法编码都会被拒绝，
 * 以保证只把格式良好的文本交给 LVGL 显示。
 *
 * @param data 待校验数据。
 * @param len  数据长度（字节）。
 * @return true 表示合法 UTF-8，false 表示非法。
 */
static bool __utf8_validate(const uint8_t *data, uint16_t len)
{
    uint16_t i = 0;

    while (i < len) {
        uint8_t first = data[i++];
        uint8_t extra = 0;

        if (first <= 0x7F) {
            continue;
        } else if (first >= 0xC2 && first <= 0xDF) {
            extra = 1;
        } else if (first >= 0xE0 && first <= 0xEF) {
            extra = 2;
            if (i >= len) {
                return false;
            }
            if ((first == 0xE0 && data[i] < 0xA0) ||
                (first == 0xED && data[i] >= 0xA0)) {
                return false;
            }
        } else if (first >= 0xF0 && first <= 0xF4) {
            extra = 3;
            if (i >= len) {
                return false;
            }
            if ((first == 0xF0 && data[i] < 0x90) ||
                (first == 0xF4 && data[i] >= 0x90)) {
                return false;
            }
        } else {
            return false;
        }

        if (i + extra > len) {
            return false;
        }
        while (extra-- > 0) {
            if ((data[i++] & 0xC0) != 0x80) {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief 将收到的 BLE 数据以 HEX + UTF-8 文本形式打印到串口日志。
 */
static void __ble_log_payload(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    PR_NOTICE("BLE received %u bytes", len);
    PR_DEBUG_RAW("BLE RX HEX:");
    for (i = 0; i < len; i++) {
        PR_DEBUG_RAW(" %02X", data[i]);
    }
    PR_DEBUG_RAW("\r\n");
    PR_NOTICE("BLE RX UTF-8: %.*s", (int)len, (const char *)data);
}

/**
 * @brief 将收到的命令行写入环形缓冲区尾部，供 UI 刷新时按序取用。
 *
 * 缓冲区已满时丢弃最旧一条，保证最新命令一定能入队（避免看板状态卡死）。
 */
static void __ble_store_message(const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len;

    if (copy_len > OPENWAIFU_BLE_MAX_MSG_LEN) {
        copy_len = OPENWAIFU_BLE_MAX_MSG_LEN;
    }

    tal_mutex_lock(sg_msg_mutex);
    memcpy(sg_ring[sg_ring_tail], data, copy_len);
    sg_ring[sg_ring_tail][copy_len] = '\0';
    sg_ring_tail = (sg_ring_tail + 1) % OPENWAIFU_BLE_QUEUE_LEN;

    if (sg_ring_count < OPENWAIFU_BLE_QUEUE_LEN) {
        sg_ring_count++;
    } else {
        /* 已满：尾部覆盖了最旧一条，头部同步前移 */
        sg_ring_head = (sg_ring_head + 1) % OPENWAIFU_BLE_QUEUE_LEN;
    }
    tal_mutex_unlock(sg_msg_mutex);
}

/**
 * @brief BLE 事件回调（运行在 BLE 协议栈任务上下文中）。
 *
 * 注意：此回调不应直接操作 LVGL 对象，UI 更新统一放到 LVGL 定时器中完成。
 */
static void __ble_event_callback(TAL_BLE_EVT_PARAMS_T *p_event)
{
    OPERATE_RET rt;
    if (p_event == NULL) {
        return;
    }

    switch (p_event->type) {
    case TAL_BLE_STACK_INIT: {
        /* 协议栈初始化完成：设置广播数据并开始广播 */
        TAL_BLE_DATA_T adv = {
            .len    = sizeof(sg_adv_data),
            .p_data = sg_adv_data,
        };
        TAL_BLE_DATA_T rsp = {
            .len    = sizeof(sg_scan_rsp_data),
            .p_data = sg_scan_rsp_data,
        };

        if (p_event->ble_event.init == OPRT_OK) {
            PR_NOTICE("BLE stack ready, starting OpenWaifu advertising");
            TUYA_CALL_ERR_LOG(tal_ble_advertising_data_set(&adv, &rsp));
            TUYA_CALL_ERR_LOG(tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM));
        } else {
            PR_ERR("BLE stack init failed: %d", p_event->ble_event.init);
        }
        break;
    }

    case TAL_BLE_EVT_PERIPHERAL_CONNECT: {
        /* 主机（电脑）连接结果 */
        sg_ble_connected = (p_event->ble_event.connect.result == OPRT_OK);
        if (sg_ble_connected) {
            PR_NOTICE("BLE connected, conn_handle=0x%04X",
                      p_event->ble_event.connect.peer.conn_handle);
        } else {
            PR_WARN("BLE connection failed, result=%d",
                    p_event->ble_event.connect.result);
        }
        break;
    }

    case TAL_BLE_EVT_DISCONNECT: {
        /* 断开连接后自动恢复广播，方便下次重连 */
        sg_ble_connected = false;
        PR_NOTICE("BLE disconnected (reason=0x%02X), restarting advertising",
                  p_event->ble_event.disconnect.reason);
        TUYA_CALL_ERR_LOG(tal_ble_advertising_start(TUYAOS_BLE_DEFAULT_ADV_PARAM));
        break;
    }

    case TAL_BLE_EVT_MTU_REQUEST: {
        PR_DEBUG("BLE MTU exchange request: %u", p_event->ble_event.exchange_mtu.mtu);
        break;
    }

    case TAL_BLE_EVT_SUBSCRIBE: {
        PR_NOTICE("BLE subscribe: notify %u->%u",
                  p_event->ble_event.subscribe.prev_notify,
                  p_event->ble_event.subscribe.cur_notify);
        break;
    }

    case TAL_BLE_EVT_WRITE_REQ: {
        /* 主机写入数据：校验 UTF-8 合法性后存入缓冲区 */
        TAL_BLE_DATA_T *report = &p_event->ble_event.write_report.report;

        if (report == NULL || report->p_data == NULL || report->len == 0) {
            PR_WARN("BLE write request with empty payload");
            break;
        }

        if (!__utf8_validate(report->p_data, report->len)) {
            PR_WARN("BLE RX payload is not valid UTF-8, dropping");
            __ble_log_payload(report->p_data, report->len);
            break;
        }

        __ble_log_payload(report->p_data, report->len);
        __ble_store_message(report->p_data, report->len);
        break;
    }

    default:
        break;
    }
}

BOOL_T openwaifu_ble_is_connected(void)
{
    return sg_ble_connected ? TRUE : FALSE;
}

BOOL_T openwaifu_ble_fetch_message(char *out, uint16_t out_size)
{
    BOOL_T has_new = FALSE;

    if (out == NULL || out_size == 0) {
        return FALSE;
    }

    tal_mutex_lock(sg_msg_mutex);
    if (sg_ring_count > 0) {
        const char *line     = sg_ring[sg_ring_head];
        uint16_t    copy_len = (uint16_t)strlen(line);
        if (copy_len > out_size - 1) {
            copy_len = out_size - 1;
        }
        memcpy(out, line, copy_len);
        out[copy_len] = '\0';
        sg_ring_head  = (sg_ring_head + 1) % OPENWAIFU_BLE_QUEUE_LEN;
        sg_ring_count--;
        has_new = TRUE;
    }
    tal_mutex_unlock(sg_msg_mutex);

    return has_new;
}

OPERATE_RET openwaifu_ble_init(void)
{
    OPERATE_RET rt;

    /* 消息缓冲区互斥锁：BLE 任务写、LVGL 任务读 */
    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&sg_msg_mutex));

    /* 蓝牙依赖 KV 存储、软件定时器与工作队列 */
    TUYA_CALL_ERR_RETURN(tal_kv_init(&(tal_kv_cfg_t){
        .seed = "openwaifu-seed-01",
        .key  = "openwaifu-key-001",
    }));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_init());
    TUYA_CALL_ERR_RETURN(tal_workq_init());

    /* 以从机角色初始化蓝牙协议栈，栈就绪后在回调里启动广播 */
    return tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, __ble_event_callback);
}
