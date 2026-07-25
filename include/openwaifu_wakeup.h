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
 * 语音交互阶段（供 UI 驱动虚拟形象）：
 * - IDLE：无语音交互进行中。
 * - CAPTURING：按键触发的录音进行中（UI 展示 Confused 形象）。
 * - WAITING：录音已结束，等待守护进程返回 TTS 内容（UI 展示 Thinking 形象）。
 */
typedef enum {
    OPENWAIFU_VOICE_IDLE = 0,
    OPENWAIFU_VOICE_CAPTURING,
    OPENWAIFU_VOICE_WAITING,
} openwaifu_voice_phase_t;

/**
 * @brief Initialize button-triggered audio capture and TTS playback.
 */
OPERATE_RET openwaifu_wakeup_init(void);

/**
 * @brief 查询当前语音交互阶段（等待超时后自动回落到 IDLE）。
 */
openwaifu_voice_phase_t openwaifu_wakeup_voice_phase(void);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_WAKEUP_H__ */
