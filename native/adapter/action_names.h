// SPDX-License-Identifier: GPL-3.0-only
// action_dedup (session_fixes.c, GitHub #323): a free name for a controller action whose name or localized name is
// already used in its action set. No OpenXR types, so the host tests can compile it.
#pragma once
#include <stdio.h>
#include <string.h>

static size_t action_name_len(const char *s, size_t max) {  // strnlen (not in plain C11)
    size_t n = 0;
    while (n < max && s[n]) ++n;
    return n;
}

// taken(ctx, name, localized): 1 when `name` or `localized` is already used in the set.
typedef int (*action_name_taken_fn)(void *ctx, const char *name, const char *localized);

// Writes "<name>_N" / "<localized> N" (N = 2..99, the first pair not taken; cut to fit name_size/localized_size
// including the terminator) and returns 1; 0 when the original names are free or no free pair exists.
static int action_unique_names(const char *name, const char *localized, action_name_taken_fn taken, void *ctx,
                               char *out_name, size_t name_size, char *out_localized, size_t localized_size) {
    if (!taken(ctx, name, localized) || name_size < 8 || localized_size < 8) return 0;
    for (int n = 2; n < 100; ++n) {
        char suffix[8], lsuffix[8];
        snprintf(suffix, sizeof(suffix), "_%d", n);
        snprintf(lsuffix, sizeof(lsuffix), " %d", n);
        size_t keep = action_name_len(name, name_size - 1);
        if (keep + strlen(suffix) > name_size - 1) keep = name_size - 1 - strlen(suffix);
        snprintf(out_name, name_size, "%.*s%s", (int)keep, name, suffix);
        size_t lkeep = action_name_len(localized, localized_size - 1);
        if (lkeep + strlen(lsuffix) > localized_size - 1) lkeep = localized_size - 1 - strlen(lsuffix);
        snprintf(out_localized, localized_size, "%.*s%s", (int)lkeep, localized, lsuffix);
        if (!taken(ctx, out_name, out_localized)) return 1;
    }
    return 0;
}
