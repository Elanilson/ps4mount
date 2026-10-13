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
