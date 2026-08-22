#pragma once
#include "lvgl.h"

/* Ark Pixel 12px (SIL Open Font License, see OFL-ark-pixel.txt), converted at
 * 1 bit per pixel. A pixel face needs no anti-aliasing, so a full CJK set
 * costs about a third of what a greyscale vector font would - and it suits a
 * screen that is otherwise all hard edges.
 *
 * Covers ASCII, CJK Unified Ideographs, CJK punctuation and fullwidth forms:
 * 18563 glyphs, so an unusual character in a reply still renders. */
LV_FONT_DECLARE(lv_font_pixel_12);
