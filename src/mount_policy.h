/* SPDX-License-Identifier: GPL-3.0-or-later
 * Conservative nullfs ownership rule, shared by PS5 runtime and host tests. */
#pragma once
#include <string.h>

static inline int mount_layer_is_owned(const char *type, const char *source, const char *expected) {
    return type && source && expected && !strcmp(type, "nullfs") && !strcmp(source, expected);
}
