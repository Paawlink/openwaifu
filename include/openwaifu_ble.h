/**
 * @file openwaifu_ble.h
 * @brief OpenWaifu BLE 从机（Peripheral / GATT Server）模块接口。
 *
 * 本模块负责初始化 BLE 协议栈、以名称 "OpenWaifu" 对外广播，并接收电脑端
 * （OpenWaifuD 守护进程）通过标准 GATT Write 特征写入的 UTF-8 命令行。
 * 每条命令行经 UTF-8 合法性校验后按“先进先出”暂存在内部环形缓冲区，供 UI
 * 模块轮询取用、解析为会话看板的增删改操作。
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 */

#ifndef __OPENWAIFU_BLE_H__
#define __OPENWAIFU_BLE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单条消息的最大字节数（UTF-8 编码后，需与守护进程侧保持一致）。 */
#define OPENWAIFU_BLE_MAX_MSG_LEN 240

/**
 * @brief 主机订阅 Notify 特征时的回调类型。
 *
 * 回调运行在 BLE 协议栈任务上下文中，仅适合做轻量操作
 * （如立即上报一次状态），不可操作 LVGL 对象。
 */
typedef void (*OPENWAIFU_BLE_SUBSCRIBE_CB)(void);

/**
 * @brief 初始化 BLE 从机并开始广播。
 *
 * 内部会完成 KV 存储、软件定时器、工作队列以及蓝牙协议栈的初始化，
 * 协议栈就绪后自动设置广播数据并启动广播。
 *
 * @return OPRT_OK 表示初始化成功，其他值表示失败。
 */
OPERATE_RET openwaifu_ble_init(void);

/**
 * @brief 查询当前 BLE 是否处于已连接状态。
 *
 * @return TRUE 表示已有主机（电脑）连接，FALSE 表示等待连接。
 */
BOOL_T openwaifu_ble_is_connected(void);

/**
 * @brief 取出下一条待处理的命令行（若存在）。
 *
 * 该函数线程安全，UI 模块可在 LVGL 定时器中周期性调用，按先进先出的顺序
 * 逐条取出 BLE 收到的命令行。取出后该条会从内部队列移除。
 *
 * @param[out] out      输出缓冲区，用于存放命令行文本（以 '\0' 结尾）。
 * @param[in]  out_size 输出缓冲区大小（字节）。
 * @return TRUE 表示有新命令并已拷贝到 out；FALSE 表示队列为空。
 */
BOOL_T openwaifu_ble_fetch_message(char *out, uint16_t out_size);

/**
 * @brief 注册主机订阅 Notify 时的回调。
 *
 * 电脑端（守护进程）订阅 Notify 特征后触发，用于立即回传一次设备
 * 当前状态快照（如 WiFi 状态），避免主机侧等到下次状态变化才同步。
 *
 * @param cb 订阅回调，传 NULL 表示取消注册。
 */
void openwaifu_ble_set_subscribe_cb(OPENWAIFU_BLE_SUBSCRIBE_CB cb);

/**
 * @brief 通过 Notify 特征向主机发送一行 UTF-8 文本。
 *
 * 仅在 BLE 已连接且主机已订阅 Notify 时实际发送，否则静默丢弃并
 * 返回错误。线程安全，可在任意任务上下文中调用（不可在中断中调用）。
 *
 * @param line 以 '\0' 结尾的文本行（超过 OPENWAIFU_BLE_MAX_MSG_LEN 会被拒绝）。
 * @return OPRT_OK 表示已提交发送，其他值表示未发送。
 */
OPERATE_RET openwaifu_ble_notify(const char *line);

/**
 * @brief 通过 Notify 特征发送二进制数据。
 *
 * 用于传输带协议头的音频分片。调用会与文本通知串行化，避免不同任务同时
 * 操作同一 Notify 特征。
 *
 * @param data 二进制数据。
 * @param len  数据长度，不得超过 OPENWAIFU_BLE_MAX_MSG_LEN。
 * @return OPRT_OK 表示已提交发送，其他值表示未发送。
 */
OPERATE_RET openwaifu_ble_notify_data(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_BLE_H__ */
