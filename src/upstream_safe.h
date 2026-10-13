/* SPDX-License-Identifier: GPL-3.0-or-later
 * Small host-testable subset adapted from original PS4Mount 0.4.0 settings. */
#pragma once
#include <stddef.h>
int ps4mount_safe_path(const char *in, char *out, size_t cap);
int ps4mount_url_decode(const char *src, size_t len, char *out, size_t cap);
