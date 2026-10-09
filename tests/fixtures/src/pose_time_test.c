// SPDX-License-Identifier: GPL-3.0-only
// pose_time_fix (native/adapter/pose_time.c) with synthetic clocks: which located times are moved, and where to.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#define LOG(...) ((void)0)
typedef int64_t XrTime;
typedef int64_t XrDuration;
static XrTime last_predicted_time;
static int64_t xr_time_offset;
static int xr_time_calibrated = 1, pose_time_fix = 1, pose_debug = 1;
#include "pose_time.c"

#define MS 1000000ll

static long long mono_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

// XrTime runs `offset` ahead of the monotonic clock; the frame being prepared shows two periods from now
static void frame(long long offset) {
    xr_time_offset = offset;
    last_predicted_time = mono_now() + offset + 21 * MS;
}

static void near(XrTime got, long long want) {
    long long d = (long long)got - want;
    if (d < -5 * MS || d > 5 * MS) {
        fprintf(stderr, "got %lld, want %lld (%+lld ms)\n", (long long)got, want, d / MS);
        assert(0);
    }
}

int main(void) {
    const long long offsets[] = {2564 * MS, 50 * MS, 900 * MS};  // SteamOS 0.4.5; the range seen on older builds
    for (unsigned i = 0; i < sizeof(offsets) / sizeof(*offsets); ++i) {
        long long offset = offsets[i];
        frame(offset);
        XrTime display = last_predicted_time;
        assert(pose_time_fixed(display) == display);                    // the frame's display time
        assert(pose_time_fixed(display + 30 * MS) == display + 30 * MS);  // predicted further ahead
        long long mono = mono_now();
        near(pose_time_fixed(mono), mono + offset);                     // OVRPlugin's monotonic "now"
        near(pose_time_fixed(mono - 8 * MS), mono - 8 * MS + offset);   // a monotonic time just before
        near(pose_time_fixed(100 * MS), mono + offset);                 // nonsense far in the past (XrTime 0.1 s)
    }
    frame(2564 * MS);
    assert(pose_time_fixed(last_predicted_time - 100 * MS) == last_predicted_time - 100 * MS);  // recent past: kept
    assert(pt_fixed_mono == 6 && pt_fixed_past == 3);
    last_display_period = 10 * MS;  // the diagnostics count per space and start over after each report
    pose_time_note_offset(2564 * MS);
    pose_time_note("xrLocateSpace", 1, 2, last_predicted_time - 2564 * MS);
    pose_time_note("xrLocateSpace", 1, 2, last_predicted_time);
    assert(pt_stats[0].count == 2 && pt_stats[0].min == -2564 * MS && pt_stats[0].max == 0);
    pose_time_report();
    assert(!pt_stats[0].count && !pt_fixed_mono && !pt_offset_count);
    frame(2 * MS);  // XrTime is the monotonic clock (a Quest): nothing to tell apart, nothing moved
    assert(pose_time_fixed(100 * MS) == 100 * MS);
    long long mono = mono_now();
    assert(pose_time_fixed(mono) == mono);
    frame(2564 * MS);
    pose_time_fix = 0;
    assert(pose_time_fixed(mono) == mono && pose_time_fixed(100 * MS) == 100 * MS);
    puts("ok");
    return 0;
}
