// SPDX-License-Identifier: GPL-3.0-only
// Display refresh rate for xrGetDisplayRefreshRateFB when the runtime doesn't give one (session_fixes.c). Pure, so the
// host test (tests/fixtures/src/refresh_rate_test.c) can check it.
#pragma once
#include <stdint.h>

// The rate the frame timing shows (1 / predictedDisplayPeriod of the last xrWaitFrame, whole Hz), else the
// refresh_rate setting, else 72 Hz (the Frame's rate for Android apps).
static inline float fb_refresh_rate(int64_t display_period_ns, float setting) {
    if (display_period_ns > 0) {
        double hz = 1e9 / (double)display_period_ns;
        if (hz >= 30.0 && hz <= 240.0) return (float)(int)(hz + 0.5);
    }
    return setting > 0 ? setting : 72.0f;
}
