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
