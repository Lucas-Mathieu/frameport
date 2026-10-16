// SPDX-License-Identifier: GPL-3.0-only
// action_dedup (native/adapter/action_names.h, GitHub #323): free names for duplicate controller actions.
#include <assert.h>
#include <string.h>
#include "action_names.h"

static const char *used[8][2];
static int nused;

static int taken(void *ctx, const char *name, const char *localized) {
    (void)ctx;
    for (int i = 0; i < nused; ++i)
        if (!strcmp(used[i][0], name) || !strcmp(used[i][1], localized)) return 1;
    return 0;
}

int main(void) {
    char name[64], localized[128];
    used[nused][0] = "turn"; used[nused++][1] = "Turn";   // Scared By Squares: action mapping "Turn"...
    assert(!action_unique_names("jump", "Jump", taken, NULL, name, sizeof(name), localized, sizeof(localized)));
    // ...and an axis mapping "Turn": same action name and localized name
    assert(action_unique_names("turn", "Turn", taken, NULL, name, sizeof(name), localized, sizeof(localized)));
    assert(!strcmp(name, "turn_2") && !strcmp(localized, "Turn 2"));
    // only the localized name clashes: still renamed (the runtime's manifest needs both unique)
    assert(action_unique_names("turn_axis", "Turn", taken, NULL, name, sizeof(name), localized, sizeof(localized)));
    assert(!strcmp(name, "turn_axis_2") && !strcmp(localized, "Turn 2"));
    // the next free number
    used[nused][0] = "turn_2"; used[nused++][1] = "Turn 2";
    assert(action_unique_names("turn", "Turn", taken, NULL, name, sizeof(name), localized, sizeof(localized)));
    assert(!strcmp(name, "turn_3") && !strcmp(localized, "Turn 3"));
    // a name at the length limit is cut so the suffix fits
    char longname[64];
    memset(longname, 'a', 63);
    longname[63] = 0;
    used[nused][0] = longname; used[nused++][1] = "Long";
    assert(action_unique_names(longname, "Long", taken, NULL, name, sizeof(name), localized, sizeof(localized)));
    assert(strlen(name) == 63 && !strcmp(name + 61, "_2") && !strcmp(localized, "Long 2"));
    return 0;
}
