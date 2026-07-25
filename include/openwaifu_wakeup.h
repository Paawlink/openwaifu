/**
 * @file openwaifu_wakeup.h
 * @brief OpenWaifu local wake-word detection.
 */

#ifndef __OPENWAIFU_WAKEUP_H__
#define __OPENWAIFU_WAKEUP_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start local detection of the "Ni Hao Tuya" wake word.
 *
 * This only runs the on-device VAD/KWS pipeline. It does not start an AI
 * conversation or connect to Tuya Cloud.
 */
OPERATE_RET openwaifu_wakeup_init(void);

/**
 * @brief Check whether local wake-word detection is running.
 */
BOOL_T openwaifu_wakeup_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_WAKEUP_H__ */
