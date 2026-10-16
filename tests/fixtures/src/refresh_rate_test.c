// SPDX-License-Identifier: GPL-3.0-only
// FrameBridge's xrGetDisplayRefreshRateFB fallback (native/adapter/refresh_rate.h, GitHub #306).
#include <assert.h>
#include "refresh_rate.h"

int main(void) {
    assert(fb_refresh_rate(13888888, 0) == 72.0f);   // 72 Hz frame timing
    assert(fb_refresh_rate(11111111, 0) == 90.0f);   // 90 Hz
    assert(fb_refresh_rate(8333333, 0) == 120.0f);   // 120 Hz
    assert(fb_refresh_rate(6944444, 0) == 144.0f);   // 144 Hz
    assert(fb_refresh_rate(0, 0) == 72.0f);          // no xrWaitFrame yet: never 0 Hz (PreyVR: 1000 / 0 ms per tic)
    assert(fb_refresh_rate(0, 90.0f) == 90.0f);      // ... else the refresh_rate setting
    assert(fb_refresh_rate(-5, 0) == 72.0f);
    assert(fb_refresh_rate(1000, 0) == 72.0f);       // nonsense periods (1 MHz, 1 Hz) aren't believed
    assert(fb_refresh_rate(1000000000, 80.0f) == 80.0f);
    return 0;
}
