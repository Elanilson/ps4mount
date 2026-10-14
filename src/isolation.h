/* SPDX-License-Identifier: GPL-3.0-or-later
 * Temporary, reversible hiding of PS4Mount-owned staging PKG symlinks.
 * NEVER rename mountpoints, live /user paths, actual USB PKGs or DB rows. */
#pragma once
#include <stddef.h>

typedef struct {
    int examined, changed, already_hidden, conflicts, failures;
    char target[10];
} isolation_report_t;

int isolation_status(const char *hold_root, const char *state_dir, isolation_report_t *out);
int isolation_enable(const char *hold_root, const char *state_dir, const char *target,
                     isolation_report_t *out);
int isolation_restore(const char *hold_root, const char *state_dir, isolation_report_t *out);
