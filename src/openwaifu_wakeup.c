/**
 * @file openwaifu_wakeup.c
 * @brief On-device "Ni Hao Tuya" wake-word detection.
 */

#include "tuya_cloud_types.h"
#include "tuya_ringbuf.h"

#include "tal_api.h"
#include "tkl_kws.h"
#include "tkl_vad.h"

#include "board_com_api.h"
#include "tdl_audio_manage.h"

#include "openwaifu_ble.h"
#include "openwaifu_wakeup.h"

#define OPENWAIFU_VAD_SPEECH_MIN_MS 200
#define OPENWAIFU_VAD_NOISE_MIN_MS  300
#define OPENWAIFU_AUDIO_BUFFER_SIZE  (96 * 1024)
#define OPENWAIFU_AUDIO_CHUNK_SIZE   230
#define OPENWAIFU_AUDIO_END_SILENCE_MS 350
#define OPENWAIFU_AUDIO_MAX_RECORD_MS  10000
#define OPENWAIFU_AUDIO_WAIT_SPEECH_MS 5000
#define OPENWAIFU_AUDIO_PROMPT_MS       120
#define OPENWAIFU_AUDIO_END_PROMPT_MS   90
#define OPENWAIFU_AUDIO_PROMPT_DELAY_MS 350
#define OPENWAIFU_AUDIO_PROMPT_HZ       880
#define OPENWAIFU_AUDIO_END_PROMPT_HZ   620
#define OPENWAIFU_AUDIO_PROMPT_LEVEL    5000

#define OPENWAIFU_AUDIO_MAGIC_0 'O'
#define OPENWAIFU_AUDIO_MAGIC_1 'W'
#define OPENWAIFU_AUDIO_MAGIC_2 'A'
#define OPENWAIFU_AUDIO_PKT_START 1
#define OPENWAIFU_AUDIO_PKT_DATA  2
#define OPENWAIFU_AUDIO_PKT_END   3

static TDL_AUDIO_HANDLE_T sg_audio_handle = NULL;
static volatile bool sg_wakeup_enabled = false;
static volatile bool sg_record_requested = false;
static volatile bool sg_recording = false;
static volatile bool sg_capture_busy = false;
static TDL_AUDIO_INFO_T sg_audio_info = {0};
static TUYA_RINGBUFF_T sg_audio_ring = NULL;
static MUTEX_HANDLE sg_audio_mutex = NULL;
static SEM_HANDLE sg_audio_sem = NULL;
static THREAD_HANDLE sg_audio_thread = NULL;
static uint32_t sg_audio_dropped = 0;

typedef enum {
    RECORD_STOP_SILENCE,
    RECORD_STOP_NO_SPEECH,
    RECORD_STOP_TIMEOUT,
    RECORD_STOP_DISCONNECTED,
    RECORD_STOP_SEND_FAILED,
} record_stop_reason_t;

static void __put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)(value >> 8);
}

static void __put_u32_le(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static OPERATE_RET __send_audio_start(uint32_t stream_id)
{
    uint8_t packet[14] = {
        OPENWAIFU_AUDIO_MAGIC_0, OPENWAIFU_AUDIO_MAGIC_1, OPENWAIFU_AUDIO_MAGIC_2,
        OPENWAIFU_AUDIO_PKT_START,
    };

    __put_u32_le(packet + 4, stream_id);
    __put_u16_le(packet + 8, sg_audio_info.sample_rate);
    packet[10] = (uint8_t)sg_audio_info.sample_bits;
    packet[11] = (uint8_t)sg_audio_info.sample_ch_num;
    __put_u16_le(packet + 12, 0);
    return openwaifu_ble_notify_data(packet, sizeof(packet));
}

static OPERATE_RET __send_audio_data(uint32_t stream_id, uint16_t sequence,
                                     const uint8_t *data, uint16_t len)
{
    uint8_t packet[OPENWAIFU_BLE_MAX_MSG_LEN];

    packet[0] = OPENWAIFU_AUDIO_MAGIC_0;
    packet[1] = OPENWAIFU_AUDIO_MAGIC_1;
    packet[2] = OPENWAIFU_AUDIO_MAGIC_2;
    packet[3] = OPENWAIFU_AUDIO_PKT_DATA;
    __put_u32_le(packet + 4, stream_id);
    __put_u16_le(packet + 8, sequence);
    memcpy(packet + 10, data, len);
    return openwaifu_ble_notify_data(packet, (uint16_t)(len + 10));
}

static void __send_audio_end(uint32_t stream_id, uint32_t pcm_bytes, uint32_t dropped_bytes)
{
    uint8_t packet[16] = {
        OPENWAIFU_AUDIO_MAGIC_0, OPENWAIFU_AUDIO_MAGIC_1, OPENWAIFU_AUDIO_MAGIC_2,
        OPENWAIFU_AUDIO_PKT_END,
    };

    __put_u32_le(packet + 4, stream_id);
    __put_u32_le(packet + 8, pcm_bytes);
    __put_u32_le(packet + 12, dropped_bytes);
    openwaifu_ble_notify_data(packet, sizeof(packet));
}

static uint16_t __pcm_frame_bytes(void)
{
    return (uint16_t)((sg_audio_info.sample_bits / 8) * sg_audio_info.sample_ch_num);
}

static void __play_prompt(uint16_t frequency, uint16_t duration_ms)
{
    int16_t samples[160 * 2];
    uint32_t frames_left;
    uint32_t phase = 0;
    uint32_t phase_step;

    if (sg_audio_info.sample_bits != 16 || sg_audio_info.sample_rate == 0 ||
        sg_audio_info.sample_ch_num == 0 || sg_audio_info.sample_ch_num > 2) {
        PR_WARN("Wake prompt skipped: unsupported PCM %u Hz/%u bit/%u ch",
                sg_audio_info.sample_rate, sg_audio_info.sample_bits,
                sg_audio_info.sample_ch_num);
        return;
    }

    frames_left = (uint32_t)sg_audio_info.sample_rate * duration_ms / 1000;
    phase_step = ((uint32_t)frequency << 16) / sg_audio_info.sample_rate;
    PR_NOTICE("Wake prompt begin: %u Hz, %u ms", frequency, duration_ms);

    while (frames_left > 0) {
        uint16_t frame_count = frames_left > 160 ? 160 : (uint16_t)frames_left;
        uint16_t i;

        for (i = 0; i < frame_count; i++) {
            uint16_t p = (uint16_t)phase;
            int32_t triangle = p < 32768 ? (int32_t)p : (int32_t)(65535 - p);
            int16_t sample = (int16_t)(((triangle - 16384) * OPENWAIFU_AUDIO_PROMPT_LEVEL) /
                                       16384);
            uint16_t ch;

            for (ch = 0; ch < sg_audio_info.sample_ch_num; ch++) {
                samples[i * sg_audio_info.sample_ch_num + ch] = sample;
            }
            phase += phase_step;
        }

        if (tdl_audio_play(sg_audio_handle, (uint8_t *)samples,
                           frame_count * sg_audio_info.sample_ch_num * sizeof(int16_t)) != OPRT_OK) {
            PR_WARN("Wake prompt playback failed");
            break;
        }
        frames_left -= frame_count;
        tal_system_sleep((uint32_t)frame_count * 1000 / sg_audio_info.sample_rate);
    }
    PR_NOTICE("Wake prompt end: %u Hz", frequency);
}

static void __rearm_detection(void)
{
    OPERATE_RET vad_stop_rt = tkl_vad_stop();
    OPERATE_RET kws_rt = tkl_kws_enable();
    OPERATE_RET vad_start_rt = tkl_vad_start();

    if (vad_stop_rt == OPRT_OK && vad_start_rt == OPRT_OK && kws_rt == OPRT_OK) {
        PR_NOTICE("Wake detection rearmed");
    } else {
        PR_ERR("Wake detection rearm failed: vad_stop=%d vad_start=%d kws=%d",
               vad_stop_rt, vad_start_rt, kws_rt);
    }
}

static void __wakeup_audio_frame(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status,
                                 uint8_t *data, uint32_t len)
{
    uint16_t frame_bytes;

    (void)type;
    (void)status;

    /* T5AI sends the AEC/VAD output to KWS inside the audio driver pipeline. */
    if (!sg_recording || data == NULL || len == 0 || sg_audio_ring == NULL) {
        return;
    }

    frame_bytes = __pcm_frame_bytes();
    if (frame_bytes == 0) {
        return;
    }
    len -= len % frame_bytes;
    if (len == 0) {
        return;
    }

    tal_mutex_lock(sg_audio_mutex);
    uint32_t written = 0;
    if (tuya_ring_buff_free_size_get(sg_audio_ring) >= len) {
        written = tuya_ring_buff_write(sg_audio_ring, data, len);
    }
    tal_mutex_unlock(sg_audio_mutex);

    if (written < len) {
        sg_audio_dropped += len - written;
    }
}

static void __wakeup_word_detected(TKL_KWS_WAKEUP_WORD_E wakeup_word)
{
    if (wakeup_word <= TKL_KWS_WAKEUP_WORD_UNKNOWN || wakeup_word >= TKL_KWS_WAKEUP_WORD_MAX) {
        return;
    }

    PR_NOTICE("Wake word detected: %d", wakeup_word);
    if (!sg_capture_busy && !sg_recording && !sg_record_requested &&
        openwaifu_ble_is_connected()) {
        sg_capture_busy = true;
        sg_record_requested = true;
        tkl_kws_disable();
        PR_NOTICE("Wake capture queued; KWS paused");
        tal_semaphore_post(sg_audio_sem);
    } else {
        PR_WARN("Wake ignored: busy=%d recording=%d requested=%d ble=%d",
                sg_capture_busy, sg_recording, sg_record_requested,
                openwaifu_ble_is_connected());
    }
}

static uint16_t __audio_ring_read(uint8_t *data, uint16_t capacity)
{
    uint16_t len;
    uint16_t frame_bytes = __pcm_frame_bytes();

    if (frame_bytes == 0) {
        return 0;
    }
    capacity -= capacity % frame_bytes;

    tal_mutex_lock(sg_audio_mutex);
    len = (uint16_t)tuya_ring_buff_read(sg_audio_ring, data, capacity);
    tal_mutex_unlock(sg_audio_mutex);
    return (uint16_t)(len - len % frame_bytes);
}

static void __audio_stream_task(void *arg)
{
    uint8_t chunk[OPENWAIFU_AUDIO_CHUNK_SIZE];
    uint32_t stream_id = 0;

    (void)arg;
    while (1) {
        tal_semaphore_wait_forever(sg_audio_sem);
        if (!sg_record_requested) {
            continue;
        }

        sg_record_requested = false;
        if (!openwaifu_ble_is_connected()) {
            sg_capture_busy = false;
            __rearm_detection();
            continue;
        }

        __play_prompt(OPENWAIFU_AUDIO_PROMPT_HZ, OPENWAIFU_AUDIO_PROMPT_MS);
        PR_NOTICE("Wake prompt complete; capture starts in %u ms",
                  OPENWAIFU_AUDIO_PROMPT_DELAY_MS);
        tal_system_sleep(OPENWAIFU_AUDIO_PROMPT_DELAY_MS);
        if (!openwaifu_ble_is_connected()) {
            sg_capture_busy = false;
            __rearm_detection();
            continue;
        }

        stream_id++;
        if (__send_audio_start(stream_id) != OPRT_OK) {
            PR_ERR("Wake stream %u start notification failed", stream_id);
            sg_capture_busy = false;
            __rearm_detection();
            continue;
        }

        tal_mutex_lock(sg_audio_mutex);
        tuya_ring_buff_reset(sg_audio_ring);
        tal_mutex_unlock(sg_audio_mutex);
        sg_audio_dropped = 0;
        sg_recording = true;

        uint16_t sequence = 0;
        uint32_t pcm_bytes = 0;
        uint32_t start_ms = (uint32_t)tal_system_get_millisecond();
        uint32_t silence_start_ms = 0;
        bool wake_phrase_ended = false;
        bool user_speech_started = false;
        bool last_speech = false;
        record_stop_reason_t stop_reason = RECORD_STOP_TIMEOUT;

        PR_NOTICE("Wake stream %u recording started: %u Hz/%u bit/%u ch",
                  stream_id, sg_audio_info.sample_rate, sg_audio_info.sample_bits,
                  sg_audio_info.sample_ch_num);

        while (sg_recording) {
            uint32_t now = (uint32_t)tal_system_get_millisecond();
            bool speech = (tkl_vad_get_status() == TKL_VAD_STATUS_SPEECH);

            if (speech != last_speech) {
                PR_NOTICE("Wake stream %u VAD %s at %u ms", stream_id,
                          speech ? "speech" : "silence", now - start_ms);
                last_speech = speech;
            }

            if (!wake_phrase_ended) {
                wake_phrase_ended = !speech;
            } else if (!user_speech_started) {
                user_speech_started = speech;
                if (!user_speech_started && now - start_ms >= OPENWAIFU_AUDIO_WAIT_SPEECH_MS) {
                    stop_reason = RECORD_STOP_NO_SPEECH;
                    sg_recording = false;
                }
            } else if (speech) {
                silence_start_ms = 0;
            } else if (silence_start_ms == 0) {
                silence_start_ms = now;
            } else if (now - silence_start_ms >= OPENWAIFU_AUDIO_END_SILENCE_MS) {
                stop_reason = RECORD_STOP_SILENCE;
                sg_recording = false;
            }

            if (now - start_ms >= OPENWAIFU_AUDIO_MAX_RECORD_MS) {
                stop_reason = RECORD_STOP_TIMEOUT;
                sg_recording = false;
            } else if (!openwaifu_ble_is_connected()) {
                stop_reason = RECORD_STOP_DISCONNECTED;
                sg_recording = false;
            }

            uint16_t len = __audio_ring_read(chunk, sizeof(chunk));
            if (len > 0) {
                if (__send_audio_data(stream_id, sequence++, chunk, len) != OPRT_OK) {
                    stop_reason = RECORD_STOP_SEND_FAILED;
                    sg_recording = false;
                } else {
                    pcm_bytes += len;
                }
                tal_system_sleep(2);
            } else {
                tal_system_sleep(5);
            }
        }

        while (openwaifu_ble_is_connected()) {
            uint16_t len = __audio_ring_read(chunk, sizeof(chunk));
            if (len == 0) {
                break;
            }
            if (__send_audio_data(stream_id, sequence++, chunk, len) != OPRT_OK) {
                break;
            }
            pcm_bytes += len;
            tal_system_sleep(2);
        }
        __send_audio_end(stream_id, pcm_bytes, sg_audio_dropped);
        PR_NOTICE("Wake stream %u ended: reason=%d duration=%u ms packets=%u bytes=%u dropped=%u",
                  stream_id, stop_reason,
                  (uint32_t)tal_system_get_millisecond() - start_ms,
                  sequence, pcm_bytes, sg_audio_dropped);
        __play_prompt(OPENWAIFU_AUDIO_END_PROMPT_HZ, OPENWAIFU_AUDIO_END_PROMPT_MS);
        sg_capture_busy = false;
        __rearm_detection();
    }
}

OPERATE_RET openwaifu_wakeup_init(void)
{
    OPERATE_RET rt = OPRT_OK;
    bool vad_inited = false;
    bool kws_inited = false;
    TKL_VAD_CONFIG_T vad_config = {0};

    TUYA_CALL_ERR_RETURN(tdl_audio_find(AUDIO_CODEC_NAME, &sg_audio_handle));
    TUYA_CALL_ERR_RETURN(tdl_audio_get_info(sg_audio_handle, &sg_audio_info));
    PR_NOTICE("Wake audio config: %u Hz/%u bit/%u ch frame=%u bytes ring=%u bytes chunk=%u bytes",
              sg_audio_info.sample_rate, sg_audio_info.sample_bits,
              sg_audio_info.sample_ch_num, sg_audio_info.frame_size,
              OPENWAIFU_AUDIO_BUFFER_SIZE, OPENWAIFU_AUDIO_CHUNK_SIZE);

    vad_config.sample_rate       = sg_audio_info.sample_rate;
    vad_config.channel_num       = sg_audio_info.sample_ch_num;
    vad_config.speech_min_ms     = OPENWAIFU_VAD_SPEECH_MIN_MS;
    vad_config.noise_min_ms      = OPENWAIFU_VAD_NOISE_MIN_MS;
    vad_config.frame_duration_ms = 20;
    vad_config.scale             = 1.0f;

    TUYA_CALL_ERR_RETURN(tuya_ring_buff_create(OPENWAIFU_AUDIO_BUFFER_SIZE,
                                                OVERFLOW_PSRAM_STOP_TYPE, &sg_audio_ring));
    TUYA_CALL_ERR_RETURN(tal_mutex_create_init(&sg_audio_mutex));
    TUYA_CALL_ERR_RETURN(tal_semaphore_create_init(&sg_audio_sem, 0, 64));

    THREAD_CFG_T thread_cfg = {
        .priority = THREAD_PRIO_4,
        .stackDepth = 4 * 1024,
        .thrdname = "wake_audio",
        .psram_mode = 1,
    };
    TUYA_CALL_ERR_RETURN(tal_thread_create_and_start(&sg_audio_thread, NULL, NULL,
                                                      __audio_stream_task, NULL, &thread_cfg));

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
