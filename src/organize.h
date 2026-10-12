/* SPDX-License-Identifier: GPL-3.0-or-later
 * Separate per-title USB-only move/rollback engine. Never touches PS5 app db. */
#pragma once
#include <sys/param.h>
#include <stddef.h>

typedef struct {
    char root[PATH_MAX], tid[10];
    char app_source[PATH_MAX], app_destination[PATH_MAX];
    char patch_source[PATH_MAX], patch_destination[PATH_MAX];
} organization_t;

/* 0=no journal, 1=journal exists (including corrupt), -1=cannot inspect safely */
int org_pending(const char *root);
/* 0 success, -1 error; on error a journal may remain for manual recovery. */
int org_apply(const organization_t *r, char *message, size_t cap);
/* READ-only human-readable description and USB journal file status. */
int org_inspect(const char *root, char *message, size_t cap);
/* Explicit rollback ONLY, never automatically during startup. */
int org_rollback(const char *root, char *message, size_t cap);
