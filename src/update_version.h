/* SPDX-License-Identifier: GPL-3.0-or-later
 * Strict numeric APP_VER handling: 01.10 > 01.09, never compare filenames. */
#pragma once
#include <stddef.h>

static int ps4mount_parse_app_ver(const char *v, int parts[4]) {
    if (!v || !*v) return -1;
    for (int i = 0; i < 4; ++i) parts[i] = 0;
    int index = 0, digits = 0;
    for (const char *p = v; *p; ++p) {
        if (*p == '.') {
            if (!digits || index >= 3) return -1;
            ++index; digits = 0;
        } else if (*p >= '0' && *p <= '9') {
            if (++digits > 4) return -1;
            parts[index] = parts[index] * 10 + (*p - '0');
            if (parts[index] > 9999) return -1;
        } else return -1;
    }
    return index >= 1 && digits ? 0 : -1;
}

/* -2 means invalid input, otherwise signed numeric comparison. */
static int ps4mount_compare_app_ver(const char *a, const char *b) {
    int va[4], vb[4];
    if (ps4mount_parse_app_ver(a, va) || ps4mount_parse_app_ver(b, vb)) return -2;
    for (int i = 0; i < 4; ++i) {
        if (va[i] > vb[i]) return 1;
        if (va[i] < vb[i]) return -1;
    }
    return 0;
}
