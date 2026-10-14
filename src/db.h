/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * app.db / appinfo.db access. */
#pragma once

#include <stdint.h>

#include "pkg.h"

/* Read only: the system databases are written by the system alone. */
int db_is_registered(const char *title_id);

/* True only after a successful, recent read-only app.db query. */
int db_snapshot_healthy(void);

/* Read-only DLC registration lookup in addcont.db. Result per content ID:
 * 1 = exact content ID record found; 0 = no record in recognized schema;
 * -1 = DB unavailable, changing, or schema unknown. Never writes system DBs. */
void db_dlc_registration(const char *const *content_ids, int n, int *states);

/* Logs selected CUSA only, including non-sensitive table/schema metadata and
 * matching DLC rows. SQLite READONLY, URI mode=ro&nolock=1. Returns count,
 * -1 when DB is unavailable, or -2 when no matching schema exists. */
int db_log_addcont_title(const char *title_id);
