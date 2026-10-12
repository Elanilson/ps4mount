/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Safety-preview: read-only status checks for the PS5 application database.
 * The previous direct app.db/appinfo.db writers were intentionally removed.
 * Console title registration is disabled in this release's default build. */
#include "db.h"
#include <stdio.h>
#include "sqlite3.h"

#ifndef APP_DB
#define APP_DB "/system_data/priv/mms/app.db"
#endif

/* Distinguish absent titles (0) from ANY failed DB lookup (-1). */
int db_registered_status(const char *title_id) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(APP_DB, &db, SQLITE_OPEN_READONLY, NULL);
    if (rc != SQLITE_OK) {
        printf("[safe][db] open read-only %s: rc=%d %s\n", APP_DB, rc,
               db ? sqlite3_errmsg(db) : "no handle");
        sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);
    sqlite3_stmt *st = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM tbl_contentinfo WHERE titleId=? LIMIT 1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_bind_text(st, 1, title_id, -1, SQLITE_TRANSIENT);
        if (rc == SQLITE_OK) rc = sqlite3_step(st);
    }
    int result = rc == SQLITE_ROW ? 1 : rc == SQLITE_DONE ? 0 : -1;
    if (result == -1)
        printf("[safe][db] query %s FAILED: rc=%d %s\n", title_id, rc, sqlite3_errmsg(db));
    sqlite3_finalize(st);
    sqlite3_close(db);
    return result;
}

int db_is_registered(const char *title_id) { return db_registered_status(title_id) == 1; }

/* Never write system databases directly in the safe preview. */
int db_register(const db_title_t *t) {
    (void)t;
    printf("[safe][db] direct SQLite registration BLOCKED\n");
    return -1;
}
int db_unregister(const char *title_id) {
    (void)title_id;
    printf("[safe][db] direct SQLite unregistration BLOCKED\n");
    return -1;
}
