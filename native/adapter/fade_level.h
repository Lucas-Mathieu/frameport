// SPDX-License-Identifier: GPL-3.0-only
// fade_quad.c: how dark the fade quad is for a layer's colour scale (host-tested: tests/fixtures/src/fade_level_test.c).
#pragma once

#define FADE_LEVELS 16

// 0 = no fade (the picture is shown as it is), else 1..FADE_LEVELS = black at level / FADE_LEVELS opacity. The fade's
// opacity is 1 - the average of the red, green and blue scale (games fade all three alike); 0.99 and above count as
// "not dimmed", so a rounding scale never leaves a faint veil.
static inline int fade_level_of(float r, float g, float b) {
    float scale = (r + g + b) / 3.0f;
    if (!(scale < 0.99f)) return 0;  // also NaN
    if (scale < 0.0f) scale = 0.0f;
    int level = (int)((1.0f - scale) * FADE_LEVELS + 0.5f);
    return level > FADE_LEVELS ? FADE_LEVELS : level;
}
