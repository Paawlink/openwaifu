#!/usr/bin/env python3
"""Generate LVGL embedded-PNG asset .c files for the plugin icons shown in the
right-hand session list.

Each source PNG (27x27, transparent) is embedded verbatim and decoded by LVGL's
lodepng at runtime, matching the existing avatar frame assets under src/assets/.
"""
import os
import struct

SRC = "/Users/turou/Downloads/AI icon"
DST = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "assets")

# (asset_name, source_png, human_note)
ICONS = [
    ("icon_claude", "claude_code_icon_27x27 (1).png", "Claude Code"),
    ("icon_openai", "openai_icon_27x27_transparent.png", "OpenAI Codex"),
    ("icon_opencode", "opencode_icon_27x27_alpha.png", "OpenCode"),
    ("icon_qoder", "qoder_icon_27x27_alpha.png", "Qoder"),
]

TEMPLATE = """#if defined(LV_LVGL_H_INCLUDE_SIMPLE)
#include "lvgl.h"
#elif defined(LV_BUILD_TEST)
#include "../lvgl.h"
#else
#include "lvgl/lvgl.h"
#endif

/* Generated from {src}: embedded PNG decoded by LVGL lodepng ({note} plugin icon). */

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

#ifndef LV_ATTRIBUTE_IMG_{upper}
#define LV_ATTRIBUTE_IMG_{upper}
#endif

static const
LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST LV_ATTRIBUTE_IMG_{upper}
uint8_t {name}_map[] = {{
{body}
}};

const lv_image_dsc_t {name} = {{
  .header.magic = LV_IMAGE_HEADER_MAGIC,
  .header.cf = LV_COLOR_FORMAT_RAW_ALPHA,
  .header.flags = 0,
  .header.w = {w},
  .header.h = {h},
  .header.stride = 0,
  .data_size = sizeof({name}_map),
  .data = {name}_map,
}};
"""


def fmt_bytes(data):
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ",".join("0x%02x" % b for b in chunk) + ",")
    return "\n".join(lines)


def main():
    dst_dir = os.path.normpath(DST)
    for name, fn, note in ICONS:
        with open(os.path.join(SRC, fn), "rb") as f:
            data = f.read()
        w, h = struct.unpack(">II", data[16:24])
        out = TEMPLATE.format(
            src=fn, note=note, upper=name.upper(), name=name,
            body=fmt_bytes(data), w=w, h=h,
        )
        dst = os.path.join(dst_dir, name + ".c")
        with open(dst, "w") as f:
            f.write(out)
        print("wrote %s (%dx%d, %d bytes)" % (dst, w, h, len(data)))


if __name__ == "__main__":
    main()
