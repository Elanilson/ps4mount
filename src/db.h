/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * app.db / appinfo.db access. */
#pragma once

#include <stdint.h>

#include "pkg.h"

typedef struct {
    const pkg_t *pkg;
    const sfo_t *sfo;       /* patch param.sfo when an update is present */
    const char *title;      /* TITLE from param.sfo */
    uint64_t size;          /* app + patch pkg sizes */
    int64_t icon_ts, pic_ts; /* appmeta file mtimes, 0 when missing */
    uint64_t install_version;
} db_title_t;

/* 1 registered, 0 not registered, -1 DB read/lock/schema error. */
int db_registered_status(const char *title_id);
int db_is_registered(const char *title_id);
/* DISABLED in safety build: always returns -1, never writes to app.db/appinfo.db. */
int db_register(const db_title_t *t);
/* DISABLED in safety build: always returns -1, never deletes DB rows. */
int db_unregister(const char *title_id);
