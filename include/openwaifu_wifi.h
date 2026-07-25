/**
 * @file openwaifu_wifi.h
 * @brief OpenWaifu WiFi Station 模块接口。
 *
 * 本模块负责 WiFi STA 模式的初始化、按 BLE 下发的凭据连接热点、
 * 凭据的 KV 持久化与开机自动重连，并在状态变化时通过 BLE Notify
 * 通道把 WiFi 状态回传给电脑端守护进程（OpenWaifuD）。
 *
 * 状态上报行格式（与守护进程侧 parse_device_notification 对应）：
 *   W|<st>|<detail>
 * 其中 <st> 为单字符状态码：
 *   I=未配置  C=连接中  G=已连接(detail 为 IP)  F=连接失败  D=已断开
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#ifndef __OPENWAIFU_WIFI_H__
#define __OPENWAIFU_WIFI_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SSID / 密码的最大长度（字节，UTF-8 编码后，与守护进程侧校验一致）。 */
#define OPENWAIFU_WIFI_SSID_MAX 32
#define OPENWAIFU_WIFI_PASS_MAX 64

/**
 * @brief 初始化 WiFi STA 模块。
 *
 * 完成 tal_wifi 初始化并切换到 Station 模式；若 KV 中存在已保存的
 * 凭据，则自动发起连接。需在 openwaifu_ble_init 之后调用（依赖 KV）。
 *
 * @return OPRT_OK 表示初始化成功，其他值表示失败。
 */
OPERATE_RET openwaifu_wifi_init(void);

/**
 * @brief 按 BLE 下发的配网命令连接 WiFi。
 *
 * 入参为守护进程百分号编码后的字段（%25=% %7C=| %0D=CR %0A=LF），
 * 内部先做 %XX 解码再发起连接。连接成功后凭据才会写入 KV 持久化。
 *
 * 该函数发起的是异步连接（tal_wifi_station_connect），结果通过
 * WiFi 事件回调上报，可在 LVGL 任务上下文中安全调用。
 *
 * @param ssid_esc 百分号编码后的 SSID（非空）。
 * @param pass_esc 百分号编码后的密码（开放网络可为空字符串）。
 */
void openwaifu_wifi_provision(const char *ssid_esc, const char *pass_esc);

/**
 * @brief 忘记网络：断开当前 WiFi 连接并清除 KV 中持久化的凭据。
 *
 * 对应 BLE 下行命令 "F"。执行后状态回到 I（未配置）并经 Notify
 * 上报；后续不会再自动重连，直到收到新的配网命令。
 * 可在 LVGL 任务上下文中安全调用。
 */
void openwaifu_wifi_forget(void);

/**
 * @brief 查询当前 WiFi 是否处于已连接（已获取 IP）状态。
 *
 * 线程安全，UI 模块可在 LVGL 定时器中周期性调用以切换 WiFi 图标显隐。
 *
 * @return TRUE 表示已连接，FALSE 表示未连接。
 */
BOOL_T openwaifu_wifi_is_connected(void);

/**
 * @brief 立即通过 BLE Notify 上报一次当前 WiFi 状态。
 *
 * 供 BLE 模块在主机订阅 Notify 时回调，保证守护进程一订阅就能
 * 拿到设备当前的 WiFi 状态快照。
 */
void openwaifu_wifi_report_status(void);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_WIFI_H__ */
