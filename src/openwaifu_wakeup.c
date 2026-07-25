/**
 * @file openwaifu_wakeup.c
 * @brief On-device "Ni Hao Tuya" wake-word detection.
 */

#include "tuya_cloud_types.h"

#include "tal_api.h"
#include "tkl_kws.h"
#include "tkl_vad.h"

#include "board_com_api.h"
#include "tdl_audio_manage.h"

#include "openwaifu_wakeup.h"

#define OPENWAIFU_VAD_SPEECH_MIN_MS 200
#define OPENWAIFU_VAD_NOISE_MIN_MS  1000

static TDL_AUDIO_HANDLE_T sg_audio_handle = NULL;
static volatile bool sg_wakeup_enabled = false;

static void __wakeup_audio_frame(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status,
                                 uint8_t *data, uint32_t len)
{
    (void)type;
    (void)status;
    (void)data;
    (void)len;

    /* T5AI sends the AEC/VAD output to KWS inside the audio driver pipeline. */
}

static void __wakeup_word_detected(TKL_KWS_WAKEUP_WORD_E wakeup_word)
{
    if (wakeup_word == TKL_KWS_WAKEUP_NIHAO_TUYA) {
        PR_NOTICE("Wake word detected: Ni Hao Tuya");
    }
}

OPERATE_RET openwaifu_wakeup_init(void)
{
    OPERATE_RET rt = OPRT_OK;
    bool vad_inited = false;
    bool kws_inited = false;
    TDL_AUDIO_INFO_T audio_info = {0};
    TKL_VAD_CONFIG_T vad_config = {0};

    TUYA_CALL_ERR_RETURN(tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio_handle));
    TUYA_CALL_ERR_RETURN(tdl_audio_get_info(sg_audio_handle, &audio_info));

    vad_config.sample_rate       = audio_info.sample_rate;
    vad_config.channel_num       = audio_info.sample_ch_num;
    vad_config.speech_min_ms     = OPENWAIFU_VAD_SPEECH_MIN_MS;
    vad_config.noise_min_ms      = OPENWAIFU_VAD_NOISE_MIN_MS;
    vad_config.frame_duration_ms = 20;
    vad_config.scale             = 1.0f;

    TUYA_CALL_ERR_RETURN(tdl_audio_open(sg_audio_handle, __wakeup_audio_frame));

    rt = tkl_vad_init(&vad_config);
    if (rt != OPRT_OK) {
        goto init_failed;
    }
    vad_inited = true;

    rt = tkl_kws_init();
    if (rt != OPRT_OK) {
        goto init_failed;
    }
    kws_inited = true;

    rt = tkl_kws_reg_wakeup_cb(__wakeup_word_detected);
    if (rt != OPRT_OK) {
        goto init_failed;
    }

    rt = tkl_vad_start();
    if (rt != OPRT_OK) {
        goto init_failed;
    }

    rt = tkl_kws_enable();
    if (rt != OPRT_OK) {
        goto init_failed;
    }

    sg_wakeup_enabled = true;
    PR_NOTICE("Local wake word ready: Ni Hao Tuya");
    return OPRT_OK;

init_failed:
    sg_wakeup_enabled = false;
    if (kws_inited) {
        tkl_kws_disable();
    }
    if (vad_inited) {
        tkl_vad_stop();
        tkl_vad_deinit();
    }
    tdl_audio_close(sg_audio_handle);
    sg_audio_handle = NULL;
    return rt;
}

BOOL_T openwaifu_wakeup_is_enabled(void)
{
    return sg_wakeup_enabled ? TRUE : FALSE;
}
