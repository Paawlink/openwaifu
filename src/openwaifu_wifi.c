/**
 * @file openwaifu_wifi.c
 * @brief OpenWaifu WiFi Station 模块实现。
 *
 * 连接流程（参考 TuyaOpen WiFi Station 教程 / examples/wifi/sta）：
 *   tal_wifi_init(事件回调) -> tal_wifi_set_work_mode(WWM_STATION)
 *   -> tal_wifi_station_connect(ssid, pass)（异步）
 * 连接结果通过 WF_EVENT_E 事件回调通知：
 *   WFE_CONNECTED      -> 取 IP、持久化凭据、上报 G|<ip>
 *   WFE_CONNECT_FAILED -> 上报 F
 *   WFE_DISCONNECTED   -> 上报 D，并用已保存凭据自动重连一次；
 *                         若凭据已被“忘记”清空则回到 I（未配置）
 *
 * 状态经 openwaifu_ble_notify() 以 "W|<st>|<detail>" 行回传守护进程。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#include "openwaifu_wifi.h"

#include <stdio.h>
#include <string.h>

#include "tal_api.h"
#include "tal_wifi.h"

#include "openwaifu_ble.h"

/***********************************************************
 *************************宏定义****************************
 ***********************************************************/
/** 凭据持久化使用的 KV key。 */
#define OW_WIFI_KV_KEY "ow_wifi_cred"

/***********************************************************
 ***********************类型定义****************************
 ***********************************************************/
/** WiFi 模块内部状态（用于向守护进程上报单字符状态码）。 */
typedef enum {
    OW_WIFI_IDLE = 0,     /* 未配置 / 未连接过 -> 'I' */
    OW_WIFI_CONNECTING,   /* 正在连接          -> 'C' */
    OW_WIFI_CONNECTED,    /* 已连接（有 IP）   -> 'G' */
    OW_WIFI_FAILED,       /* 连接失败          -> 'F' */
    OW_WIFI_DISCONNECTED, /* 连接后断开        -> 'D' */
} ow_wifi_state_e;

/** KV 中保存的凭据结构（整体二进制存取）。 */
typedef struct {
    char ssid[OPENWAIFU_WIFI_SSID_MAX + 1];
    char pass[OPENWAIFU_WIFI_PASS_MAX + 1];
} ow_wifi_cred_t;

/***********************************************************
 ***********************变量定义****************************
 ***********************************************************/
static volatile ow_wifi_state_e sg_wifi_state = OW_WIFI_IDLE; /* 当前状态 */
static char           sg_wifi_ip[16] = {0}; /* 已连接时的 IPv4 地址字符串 */
static ow_wifi_cred_t sg_cred        = {0}; /* 最近一次尝试连接的凭据 */
static bool           sg_wifi_ready  = false; /* tal_wifi 初始化是否成功 */

/***********************************************************
 ***********************函数定义****************************
 ***********************************************************/

/**
 * @brief 把内部状态映射为协议单字符状态码。
 */
static char __wifi_state_char(ow_wifi_state_e state)
{
    switch (state) {
    case OW_WIFI_CONNECTING:
        return 'C';
    case OW_WIFI_CONNECTED:
        return 'G';
    case OW_WIFI_FAILED:
        return 'F';
    case OW_WIFI_DISCONNECTED:
        return 'D';
    case OW_WIFI_IDLE:
    default:
        return 'I';
    }
}

/**
 * @brief 更新状态并通过 BLE Notify 上报（未订阅时静默跳过）。
 */
static void __wifi_set_state(ow_wifi_state_e state)
{
    sg_wifi_state = state;
    openwaifu_wifi_report_status();
}

/**
 * @brief 对守护进程百分号编码的字段做 %XX 解码。
 *
 * 非法或截断的转义序列按原样保留，保证解码永不失败。
 *
 * @param src      编码后的输入字符串。
 * @param dst      解码输出缓冲区。
 * @param dst_size 输出缓冲区大小（含结尾 '\0'）。
 */
static void __percent_decode(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;

    while (*src != '\0' && out + 1 < dst_size) {
        if (src[0] == '%' && src[1] != '\0' && src[2] != '\0') {
            char hex[3] = {src[1], src[2], '\0'};
            char *end   = NULL;
            long  val   = strtol(hex, &end, 16);

            if (end == hex + 2) {
                dst[out++] = (char)val;
                src += 3;
                continue;
            }
        }
        dst[out++] = *src++;
    }
    dst[out] = '\0';
}

/**
 * @brief 把当前凭据写入 KV（仅在连接成功后调用）。
 */
static void __wifi_save_cred(void)
{
    int rt = tal_kv_set(OW_WIFI_KV_KEY, (const uint8_t *)&sg_cred, sizeof(sg_cred));

    if (rt != OPRT_OK) {
        PR_WARN("wifi cred save failed: %d", rt);
    } else {
        PR_NOTICE("wifi cred saved, ssid=%s", sg_cred.ssid);
    }
}

/**
 * @brief 从 KV 读取已保存的凭据。
 *
 * @return true 表示读取成功且 SSID 非空。
 */
static bool __wifi_load_cred(void)
{
    uint8_t *value  = NULL;
    size_t   length = 0;
    bool     ok     = false;

    if (tal_kv_get(OW_WIFI_KV_KEY, &value, &length) != OPRT_OK || value == NULL) {
        return false;
    }

    if (length == sizeof(ow_wifi_cred_t)) {
        memcpy(&sg_cred, value, sizeof(sg_cred));
        /* 防御性收尾，避免脏数据破坏字符串边界 */
        sg_cred.ssid[OPENWAIFU_WIFI_SSID_MAX] = '\0';
        sg_cred.pass[OPENWAIFU_WIFI_PASS_MAX] = '\0';
        ok = (sg_cred.ssid[0] != '\0');
    } else {
        PR_WARN("wifi cred size mismatch (%u), dropping", (unsigned int)length);
        tal_kv_del(OW_WIFI_KV_KEY);
    }

    tal_kv_free(value);
    return ok;
}

/**
 * @brief 用当前凭据发起异步连接并切换到“连接中”状态。
 */
static void __wifi_start_connect(void)
{
    OPERATE_RET rt;

    PR_NOTICE("wifi connecting to ssid=%s", sg_cred.ssid);
    __wifi_set_state(OW_WIFI_CONNECTING);

    rt = tal_wifi_station_connect((int8_t *)sg_cred.ssid, (int8_t *)sg_cred.pass);
    if (rt != OPRT_OK) {
        PR_ERR("wifi station connect failed: %d", rt);
        __wifi_set_state(OW_WIFI_FAILED);
    }
}

/**
 * @brief WiFi 事件回调（运行在 WiFi 任务上下文中，不可操作 LVGL）。
 */
static void __wifi_event_callback(WF_EVENT_E event, void *arg)
{
    (void)arg;

    switch (event) {
    case WFE_CONNECTED: {
        NW_IP_S ip_info;

        memset(&ip_info, 0, sizeof(ip_info));
        if (tal_wifi_get_ip(WF_STATION, &ip_info) == OPRT_OK) {
            snprintf(sg_wifi_ip, sizeof(sg_wifi_ip), "%s", ip_info.ip);
        } else {
            sg_wifi_ip[0] = '\0';
        }
        PR_NOTICE("wifi connected, ip=%s", sg_wifi_ip);

        /* 连接成功后才持久化凭据，避免把错误密码写入 KV */
        __wifi_save_cred();
        __wifi_set_state(OW_WIFI_CONNECTED);
        break;
    }

    case WFE_CONNECT_FAILED: {
        PR_WARN("wifi connect failed, ssid=%s", sg_cred.ssid);
        sg_wifi_ip[0] = '\0';
        __wifi_set_state(OW_WIFI_FAILED);
        break;
    }

    case WFE_DISCONNECTED: {
        PR_WARN("wifi disconnected");
        sg_wifi_ip[0] = '\0';

        if (sg_cred.ssid[0] != '\0') {
            __wifi_set_state(OW_WIFI_DISCONNECTED);
            /* 自动重连；失败会走 WFE_CONNECT_FAILED，不会循环 */
            __wifi_start_connect();
        } else {
            /* 凭据已被“忘记”清空：回到未配置状态，不重连 */
            __wifi_set_state(OW_WIFI_IDLE);
        }
        break;
    }

    default:
        break;
    }
}

void openwaifu_wifi_provision(const char *ssid_esc, const char *pass_esc)
{
    char ssid[OPENWAIFU_WIFI_SSID_MAX + 1];
    char pass[OPENWAIFU_WIFI_PASS_MAX + 1];

    if (!sg_wifi_ready) {
        PR_WARN("wifi not ready, provision ignored");
        return;
    }
    if (ssid_esc == NULL) {
        return;
    }

    __percent_decode(ssid_esc, ssid, sizeof(ssid));
    __percent_decode(pass_esc != NULL ? pass_esc : "", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        PR_WARN("wifi provision with empty ssid, ignored");
        return;
    }

    strncpy(sg_cred.ssid, ssid, OPENWAIFU_WIFI_SSID_MAX);
    sg_cred.ssid[OPENWAIFU_WIFI_SSID_MAX] = '\0';
    strncpy(sg_cred.pass, pass, OPENWAIFU_WIFI_PASS_MAX);
    sg_cred.pass[OPENWAIFU_WIFI_PASS_MAX] = '\0';

    __wifi_start_connect();
}

void openwaifu_wifi_forget(void)
{
    ow_wifi_state_e state = sg_wifi_state;

    PR_NOTICE("wifi forget requested");

    /* 先清空凭据，保证随后的断开事件不会触发自动重连 */
    memset(&sg_cred, 0, sizeof(sg_cred));
    sg_wifi_ip[0] = '\0';
    tal_kv_del(OW_WIFI_KV_KEY);

    if (sg_wifi_ready && (state == OW_WIFI_CONNECTED || state == OW_WIFI_CONNECTING)) {
        OPERATE_RET rt = tal_wifi_station_disconnect();
        if (rt != OPRT_OK) {
            PR_WARN("wifi station disconnect failed: %d", rt);
        }
    }

    /* 立即回到未配置状态并上报；后续 WFE_DISCONNECTED 事件因凭据
     * 已清空会再次上报 I，幂等无副作用 */
    __wifi_set_state(OW_WIFI_IDLE);
}

BOOL_T openwaifu_wifi_is_connected(void)
{
    return (sg_wifi_state == OW_WIFI_CONNECTED) ? TRUE : FALSE;
}

void openwaifu_wifi_report_status(void)
{
    char line[OPENWAIFU_BLE_MAX_MSG_LEN + 1];
    ow_wifi_state_e state = sg_wifi_state;

    if (state == OW_WIFI_CONNECTED) {
        snprintf(line, sizeof(line), "W|G|%s", sg_wifi_ip);
    } else {
        snprintf(line, sizeof(line), "W|%c|", __wifi_state_char(state));
    }
    openwaifu_ble_notify(line);
}

OPERATE_RET openwaifu_wifi_init(void)
{
    OPERATE_RET rt;

    /* KV 已由 openwaifu_ble_init 初始化，这里直接初始化 WiFi 协议栈 */
    TUYA_CALL_ERR_RETURN(tal_wifi_init(__wifi_event_callback));
    TUYA_CALL_ERR_RETURN(tal_wifi_set_work_mode(WWM_STATION));
    sg_wifi_ready = true;

    /* 主机订阅 Notify 时，立即回传一次当前 WiFi 状态快照 */
    openwaifu_ble_set_subscribe_cb(openwaifu_wifi_report_status);

    /* 存在已保存凭据时开机自动重连 */
    if (__wifi_load_cred()) {
        __wifi_start_connect();
    }

    return OPRT_OK;
}
