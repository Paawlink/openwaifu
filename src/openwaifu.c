/**
 * @file openwaifu.c
 * @brief OpenWaifu application with BLE Peripheral and LVGL UI.
 *
 * This file initializes the LVGL GUI and a BLE Peripheral (GATT Server) on the
 * OpenWaifu device. The device advertises as "OpenWaifu", accepts connections
 * from a computer, and displays UTF-8 messages (including Chinese) written to
 * the Tuya common Write characteristic on the screen and in the debug log.
 *
 * @copyright Copyright (c) 2021-2024 Tuya Inc. All Rights Reserved.
 *
 */

#include "tuya_cloud_types.h"

#include "tal_api.h"
#include "tal_bluetooth.h"
#include "tkl_output.h"
#include "tkl_spi.h"
#include "tkl_system.h"

#include "lvgl.h"
#include "lv_vendor.h"
#include "board_com_api.h"
#include "openwaifu_font.h"

/***********************************************************
 *************************micro define**********************
 ***********************************************************/
#define OPENWAIFU_BLE_MAX_MSG_LEN  240
#define OPENWAIFU_BLE_ADV_SVC_UUID 0xFD50

/***********************************************************
 ***********************variable define**********************
 ***********************************************************/
static lv_obj_t    *sg_status_label  = NULL;
static lv_obj_t    *sg_message_label = NULL;
static MUTEX_HANDLE sg_msg_mutex     = NULL;
static char         sg_msg_buf[OPENWAIFU_BLE_MAX_MSG_LEN + 1];
static uint16_t     sg_msg_len       = 0;
static bool         sg_msg_pending   = false;
static bool         sg_ble_connected = false;

static uint8_t sg_adv_data[] = {
    0x02, 0x01, 0x06,
    0x03, 0x03, (uint8_t)(OPENWAIFU_BLE_ADV_SVC_UUID & 0xFF),
                  (uint8_t)(OPENWAIFU_BLE_ADV_SVC_UUID >> 8),
};

static uint8_t sg_scan_rsp_data[] = {
    0x0A, 0x09, 'O', 'p', 'e', 'n', 'W', 'a', 'i', 'f', 'u',
};

/***********************************************************
 ***********************function define**********************
 ***********************************************************/

/**
 * @brief Check whether the byte sequence is valid UTF-8.
 *
 * Truncated multi-byte sequences and overlong/surrogate encodings are rejected
 * so that LVGL only receives well-formed text.
 */
static bool __utf8_validate(const uint8_t *data, uint16_t len)
{
    uint16_t i = 0;

    while (i < len) {
        uint8_t  first = data[i++];
        uint8_t  extra = 0;

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
 * @brief Print the received BLE payload as hex dump and UTF-8 text.
 */
static void __ble_log_payload(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    PR_NOTICE("BLE received %u bytes (conn 0x%04X)", len, 0);
    PR_DEBUG_RAW("BLE RX HEX:");
    for (i = 0; i < len; i++) {
        PR_DEBUG_RAW(" %02X", data[i]);
    }
    PR_DEBUG_RAW("\r\n");
    PR_NOTICE("BLE RX UTF-8: %.*s", (int)len, (const char *)data);
}

/**
 * @brief Store the received message into the shared buffer for UI refresh.
 */
static void __ble_store_message(const uint8_t *data, uint16_t len)
{
    uint16_t copy_len = len;

    if (copy_len > OPENWAIFU_BLE_MAX_MSG_LEN) {
        copy_len = OPENWAIFU_BLE_MAX_MSG_LEN;
    }

    tal_mutex_lock(sg_msg_mutex);
    memcpy(sg_msg_buf, data, copy_len);
    sg_msg_buf[copy_len] = '\0';
    sg_msg_len = copy_len;
    sg_msg_pending = true;
    tal_mutex_unlock(sg_msg_mutex);
}

/**
 * @brief BLE event callback (runs in BLE stack task context).
 */
static void __ble_event_callback(TAL_BLE_EVT_PARAMS_T *p_event)
{
    OPERATE_RET rt;
    if (p_event == NULL) {
        return;
    }

    switch (p_event->type) {
    case TAL_BLE_STACK_INIT: {
        TAL_BLE_DATA_T adv = {
            .len = sizeof(sg_adv_data),
            .p_data = sg_adv_data,
        };
        TAL_BLE_DATA_T rsp = {
            .len = sizeof(sg_scan_rsp_data),
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

/**
 * @brief LVGL timer callback to refresh status and latest message.
 *
 * Runs in the LVGL task context, so it is safe to touch LVGL objects here.
 */
static void __ui_refresh_cb(lv_timer_t *timer)
{
    char    msg[OPENWAIFU_BLE_MAX_MSG_LEN + 1];
    uint16_t msg_len;
    bool     pending;

    (void)timer;

    tal_mutex_lock(sg_msg_mutex);
    msg_len = sg_msg_len;
    memcpy(msg, sg_msg_buf, msg_len + 1);
    pending = sg_msg_pending;
    sg_msg_pending = false;
    tal_mutex_unlock(sg_msg_mutex);

    lv_label_set_text(sg_status_label,
                      sg_ble_connected ? "BLE: Connected" : "BLE: Waiting...");

    if (pending) {
        (void)msg_len;
        lv_label_set_text(sg_message_label, msg);
    }
}

/**
 * @brief Build the LVGL user interface.
 */
static void __ui_init(void)
{
    lv_obj_t *title;

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_white(), LV_PART_MAIN);

    title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "OpenWaifu BLE");
    lv_obj_set_style_text_color(title, lv_color_black(), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    sg_status_label = lv_label_create(lv_screen_active());
    lv_label_set_text(sg_status_label, "BLE: Waiting...");
    lv_obj_set_style_text_color(sg_status_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_align(sg_status_label, LV_ALIGN_TOP_MID, 0, 60);

    sg_message_label = lv_label_create(lv_screen_active());
    lv_label_set_text(sg_message_label, "Waiting for message...");
    lv_label_set_long_mode(sg_message_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(sg_message_label, 300);
    lv_obj_set_style_text_color(sg_message_label, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_text_font(sg_message_label, openwaifu_font(), LV_PART_MAIN);
    lv_obj_align(sg_message_label, LV_ALIGN_CENTER, 0, 30);

    lv_timer_create(__ui_refresh_cb, 100, NULL);
}

/**
 * @brief Initialize BLE Peripheral stack and start advertising.
 */
static OPERATE_RET __ble_init(void)
{
    OPERATE_RET rt;
    TUYA_CALL_ERR_RETURN(tal_kv_init(&(tal_kv_cfg_t){
        .seed = "openwaifu-seed-01",
        .key  = "openwaifu-key-001",
    }));
    TUYA_CALL_ERR_RETURN(tal_sw_timer_init());
    TUYA_CALL_ERR_RETURN(tal_workq_init());

    return tal_ble_bt_init(TAL_BLE_ROLE_PERIPERAL, __ble_event_callback);
}

/**
 * @brief user_main
 */
void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;
    tal_log_init(TAL_LOG_LEVEL_DEBUG, 4096, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("Application information:");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("TuyaOpen version:    %s", OPEN_VERSION);
    PR_NOTICE("TuyaOpen commit-id:  %s", OPEN_COMMIT);
    PR_NOTICE("Platform chip:       %s", PLATFORM_CHIP);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("Platform commit-id:  %s", PLATFORM_COMMIT);

    board_register_hardware();

    TUYA_CALL_ERR_LOG(tal_mutex_create_init(&sg_msg_mutex));

    lv_vendor_init(DISPLAY_NAME);
    __ui_init();
    lv_vendor_start(5, 1024 * 8);

    if (__ble_init() != OPRT_OK) {
        PR_ERR("BLE init failed, continuing without BLE");
    } else {
        PR_NOTICE("OpenWaifu BLE Peripheral initialized");
    }
}

/**
 * @brief main
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();

    while (1) {
        tal_system_sleep(500);
    }
}
#else

static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief  task thread
 */
static void tuya_app_thread(void *arg)
{
    (void)arg;

    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
    THREAD_CFG_T thrd_param;

    memset(&thrd_param, 0, sizeof(THREAD_CFG_T));
    thrd_param.stackDepth = 1024 * 4;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "tuya_app_main";

    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif
