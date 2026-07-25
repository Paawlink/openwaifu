/**
 * @file openwaifu_wakeup.h
 * @brief OpenWaifu button-triggered audio capture.
 */

#ifndef __OPENWAIFU_WAKEUP_H__
#define __OPENWAIFU_WAKEUP_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize button-triggered audio capture and TTS playback.
 */
OPERATE_RET openwaifu_wakeup_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_WAKEUP_H__ */
