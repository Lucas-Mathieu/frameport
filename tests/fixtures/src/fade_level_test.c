// SPDX-License-Identifier: GPL-3.0-only
// fade_quad (native/adapter/fade_level.h): how dark the fade quad is for a layer's colour scale.
#include <assert.h>
#include <math.h>
#include "fade_level.h"

int main(void) {
    assert(fade_level_of(1.0f, 1.0f, 1.0f) == 0);              // not dimmed: no quad
    assert(fade_level_of(0.995f, 0.995f, 0.995f) == 0);        // rounding noise: no faint veil
    assert(fade_level_of(0.0f, 0.0f, 0.0f) == FADE_LEVELS);    // black (Batman's first frame)
    assert(fade_level_of(0.5f, 0.5f, 0.5f) == FADE_LEVELS / 2);
    assert(fade_level_of(0.97f, 0.97f, 0.97f) == 0);           // 0.48 of a level rounds to none
    assert(fade_level_of(0.95f, 0.95f, 0.95f) == 1);           // the first visible step
    assert(fade_level_of(-1.0f, -1.0f, -1.0f) == FADE_LEVELS); // out of range: clamped
    assert(fade_level_of(2.0f, 2.0f, 2.0f) == 0);              // brightened: nothing to draw
    assert(fade_level_of(NAN, 0.0f, 0.0f) == 0);               // garbage: nothing to draw
    for (int i = 0; i <= 100; ++i) {                            // monotonic: a darker scale never gives a lighter quad
        float s = i / 100.0f, t = (i + 1) / 100.0f;
        assert(fade_level_of(s, s, s) >= fade_level_of(t, t, t));
    }
    return 0;
}
