/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Read-only lookups in the system databases. */
/* The system databases are written by the system alone: ShellCore keeps them open
 * with its own cache, and rows written from outside make it flag them as corrupted. */
#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sqlite3.h"

#ifndef APP_DB
#define APP_DB "/system_data/priv/mms/app.db"
#endif

/* Titles registered in app.db, read in one query at most once a second. The file is
 * opened without locks (nolock=1): a shared lock held here, even for a moment, can make
 * a write of ShellCore fail, and ShellCore then flags the database as corrupted. */
static char (*g_titles)[10];
static int g_ntitles, g_cap;
static time_t g_read_at;
static time_t g_last_good;
static int g_read_ok;

static void refresh(void) {
    if (g_read_ok && time(NULL) == g_read_at)
        return;
    g_read_at = time(NULL);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" APP_DB "?mode=ro&nolock=1", &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) !=
        SQLITE_OK) {
        sqlite3_close(db);
        return; /* keep the last list */
    }
    sqlite3_stmt *st;
    int n = 0, ok = 0;
    if (sqlite3_prepare_v2(db, "SELECT titleId FROM tbl_contentinfo", -1, &st, NULL) == SQLITE_OK) {
        int rc;
        while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(st, 0);
            if (!t)
                continue;
            if (n == g_cap) {
                int cap = g_cap ? g_cap * 2 : 256;
                char (*grown)[10] = realloc(g_titles, sizeof(*grown) * (size_t)cap);
                if (!grown)
                    break;
                g_titles = grown;
                g_cap = cap;
            }
            snprintf(g_titles[n++], 10, "%s", t);
        }
        ok = rc == SQLITE_DONE;
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (ok) { /* a read that met a write in progress keeps the last list */
        g_ntitles = n;
        g_read_ok = 1;
        g_last_good = time(NULL);
    }
}

int db_is_registered(const char *title_id) {
    refresh();
    for (int i = 0; i < g_ntitles; i++)
        if (!strcmp(g_titles[i], title_id))
            return 1;
    return 0;
}

/* An inconclusive database read must not be mistaken for an absent game.
 * The cache is useful for display, but permission to register requires a
 * recent successful SQLite read. */
int db_snapshot_healthy(void) {
    refresh();
    return g_read_ok && g_last_good && time(NULL) - g_last_good <= 2;
}


#ifndef ADDCONT_DB
#define ADDCONT_DB "/system_data/priv/mms/addcont.db"
#endif

/* The addcont.db schema varies. Probe its content-ID columns instead of assuming
 * a particular table name. No registration is inferred from files merely existing.
 * This is diagnostic only; it does not install an entitlement or replace the
 * console installer, and it does not modify the database. */
void db_dlc_registration(const char *const *content_ids, int n, int *states) {
    if (n <= 0 || !content_ids || !states) return;
    for (int i = 0; i < n; ++i) states[i] = -1;
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" ADDCONT_DB "?mode=ro&nolock=1", &db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    /* Keep reads bounded: neither wait on nor block the service's writes. */
    sqlite3_busy_timeout(db, 0);
    sqlite3_stmt *tables = NULL;
    int schema_found = 0, query_failed = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
            -1, &tables, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }
    int table_rc;
    while ((table_rc = sqlite3_step(tables)) == SQLITE_ROW) {
        const char *tname = (const char *)sqlite3_column_text(tables, 0);
        if (!tname) continue;
        char *pragma = sqlite3_mprintf("PRAGMA table_info(\"%w\")", tname);
        sqlite3_stmt *cols = NULL;
        if (!pragma || sqlite3_prepare_v2(db, pragma, -1, &cols, NULL) != SQLITE_OK) {
            query_failed = 1;
            sqlite3_free(pragma);
            if (cols) sqlite3_finalize(cols);
            break;
        }
        sqlite3_free(pragma);
        int col_rc;
        while ((col_rc = sqlite3_step(cols)) == SQLITE_ROW) {
            const char *col = (const char *)sqlite3_column_text(cols, 1);
            if (!col || (sqlite3_stricmp(col, "contentId") &&
                         sqlite3_stricmp(col, "content_id") &&
                         sqlite3_stricmp(col, "contentid"))) continue;
            schema_found = 1;
            char *sql = sqlite3_mprintf("SELECT 1 FROM \"%w\" WHERE \"%w\"=? LIMIT 1", tname, col);
            sqlite3_stmt *q = NULL;
            if (!sql || sqlite3_prepare_v2(db, sql, -1, &q, NULL) != SQLITE_OK) {
                query_failed = 1;
                sqlite3_free(sql);
                if (q) sqlite3_finalize(q);
                break;
            }
            sqlite3_free(sql);
            for (int i = 0; i < n; ++i) {
                if (states[i] == 1) continue;
                sqlite3_reset(q);
                sqlite3_clear_bindings(q);
                sqlite3_bind_text(q, 1, content_ids[i], -1, SQLITE_TRANSIENT);
                int rc = sqlite3_step(q);
                if (rc == SQLITE_ROW) states[i] = 1;
                else if (rc == SQLITE_DONE && states[i] != 1) states[i] = 0;
                else if (rc != SQLITE_DONE) query_failed = 1;
            }
            sqlite3_finalize(q);
            if (query_failed) break;
        }
        if (col_rc != SQLITE_DONE) query_failed = 1;
        sqlite3_finalize(cols);
        if (query_failed) break;
    }
    if (table_rc != SQLITE_DONE) query_failed = 1;
    sqlite3_finalize(tables);
    sqlite3_close(db);
    if (!schema_found || query_failed)
        for (int i = 0; i < n; ++i) states[i] = -1;
}


/* Diagnostic for comparing a native-installed DLC with an unregistered USB DLC.
 * Never print the entire database: the PS5 tables may contain user/account data.
 * Only print records matching the requested CUSA in a recognized Content ID.
 * Values are restricted to technical fields; credentials, paths and blobs are omitted. */
static int diagnostic_col_safe(const char *col) {
    if (!col) return 0;
    static const char *safe[] = {
        "contentId", "content_id", "titleId", "title_id", "dirName", "dir_name",
        "contentType", "content_type", "contentPlatform", "content_platform",
        "contentVersion", "content_version", "version", "appVersion", "app_version",
        "status", "state", "installStatus", "install_status", "installed",
        "contentSize", "content_size", "size", "platform", "type", NULL
    };
    for (int i = 0; safe[i]; ++i)
        if (!sqlite3_stricmp(col, safe[i])) return 1;
    return 0;
}

static void diagnostic_print_value(sqlite3_stmt *s, int c) {
    int ty = sqlite3_column_type(s, c);
    if (ty == SQLITE_INTEGER) {
        printf("%lld", (long long)sqlite3_column_int64(s, c));
    } else if (ty == SQLITE_FLOAT) {
        printf("%.6g", sqlite3_column_double(s, c));
    } else if (ty == SQLITE_NULL) {
        printf("NULL");
    } else if (ty == SQLITE_BLOB) {
        printf("<BLOB omitted>");
    } else if (ty == SQLITE_TEXT) {
        const unsigned char *v = sqlite3_column_text(s, c);
        int n = sqlite3_column_bytes(s, c);
        int limit = n < 96 ? n : 96;
        for (int i = 0; i < limit && v; ++i) {
            unsigned char x = v[i];
            /* Stable single-line logs; never print arbitrary control sequences. */
            putchar(x >= 32 && x < 127 ? (int)x : '?');
        }
        if (n > limit) printf("...");
    } else {
        printf("<type=%d>", ty);
    }
}

int db_log_addcont_title(const char *title_id) {
    if (!title_id || strlen(title_id) != 9 || strncmp(title_id, "CUSA", 4)) return -1;
    for (int i = 4; i < 9; i++) if (title_id[i] < '0' || title_id[i] > '9') return -1;
    printf("[db-inspect] %s: BEGIN addcont.db readonly filtered CUSA; no database writes\n", title_id);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2("file:" ADDCONT_DB "?mode=ro&nolock=1", &db,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_URI, NULL) != SQLITE_OK) {
        printf("[db-inspect] %s: addcont.db could not be opened read-only\n", title_id);
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 0);
    sqlite3_stmt *tables = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'",
            -1, &tables, NULL) != SQLITE_OK) {
        printf("[db-inspect] %s: schema cannot be read\n", title_id);
        sqlite3_close(db);
        return -1;
    }
    int total_rows = 0, relevant_tables = 0, failed = 0, table_count = 0;
    int trc;
    while ((trc = sqlite3_step(tables)) == SQLITE_ROW && table_count++ < 32) {
        const char *name = (const char *)sqlite3_column_text(tables, 0);
        if (!name) continue;
        char *pragma = sqlite3_mprintf("PRAGMA table_info(\"%w\")", name);
        sqlite3_stmt *cols = NULL;
        if (!pragma || sqlite3_prepare_v2(db, pragma, -1, &cols, NULL) != SQLITE_OK) {
            failed = 1; sqlite3_free(pragma); if (cols) sqlite3_finalize(cols); break;
        }
        sqlite3_free(pragma);
        char idcol[96] = "", columns[768] = "";
        int colrc;
        while ((colrc = sqlite3_step(cols)) == SQLITE_ROW) {
            const char *cn = (const char *)sqlite3_column_text(cols, 1);
            if (!cn) continue;
            /* Print column names only, not values of private columns. */
            if (strlen(columns) + strlen(cn) + 3 < sizeof(columns)) {
                if (*columns) strcat(columns, ",");
                strcat(columns, cn);
            }
            if (!*idcol && (!sqlite3_stricmp(cn, "contentId") ||
                           !sqlite3_stricmp(cn, "content_id") ||
                           !sqlite3_stricmp(cn, "contentid")))
                snprintf(idcol, sizeof(idcol), "%s", cn);
        }
        if (colrc != SQLITE_DONE) failed = 1;
        sqlite3_finalize(cols);
        if (failed) break;
        if (!*idcol) continue; /* Only tables containing exact DLC Content IDs. */
        relevant_tables++;
        printf("[db-inspect] %s: table=%s columns=%s\n", title_id, name, columns);
        char *query = sqlite3_mprintf(
            "SELECT * FROM \"%w\" WHERE instr(\"%w\", ?) > 0 LIMIT 64", name, idcol);
        sqlite3_stmt *q = NULL;
        if (!query || sqlite3_prepare_v2(db, query, -1, &q, NULL) != SQLITE_OK) {
            failed = 1; sqlite3_free(query); if (q) sqlite3_finalize(q); break;
        }
        sqlite3_free(query);
        sqlite3_bind_text(q, 1, title_id, -1, SQLITE_TRANSIENT);
        int count = 0, rc;
        while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
            count++; total_rows++;
            printf("[db-inspect] %s row=%d table=%s", title_id, count, name);
            int nf = sqlite3_column_count(q);
            for (int c = 0; c < nf; c++) {
                const char *col = sqlite3_column_name(q, c);
                if (!diagnostic_col_safe(col)) continue;
                printf(" %s=", col);
                diagnostic_print_value(q, c);
            }
            printf("\n");
        }
        if (rc != SQLITE_DONE) failed = 1;
        sqlite3_finalize(q);
        if (failed) break;
        printf("[db-inspect] %s: table=%s matched=%d\n", title_id, name, count);
    }
    if (trc != SQLITE_DONE && table_count <= 32) failed = 1;
    sqlite3_finalize(tables);
    sqlite3_close(db);
    printf("[db-inspect] %s: END tables=%d rows=%d status=%s (content-id records, not license verification)\n",
           title_id, relevant_tables, total_rows,
           failed ? "READ_ERROR" : !relevant_tables ? "SCHEMA_UNKNOWN" : "OK");
    if (failed) return -1;
    if (!relevant_tables) return -2;
    return total_rows;
}
