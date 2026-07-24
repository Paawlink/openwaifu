/**
 * @file openwaifu_font.h
 * @brief OpenWaifu CJK font accessor.
 *
 * Exposes the Alibaba PuHuiTi 18px LVGL font (with comprehensive CJK coverage)
 * used by the OpenWaifu message display.
 */

#ifndef __OPENWAIFU_FONT_H__
#define __OPENWAIFU_FONT_H__

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Get the OpenWaifu CJK font.
 * @return Pointer to the lv_font_t capable of rendering Chinese text.
 */
const lv_font_t *openwaifu_font(void);

#ifdef __cplusplus
}
#endif

#endif /* __OPENWAIFU_FONT_H__ */
