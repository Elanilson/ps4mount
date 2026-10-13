/* Host-only read-only addcont.db compatibility regression. */
#include <assert.h>
#include <stdio.h>
#include "sqlite3.h"
#include "db.h"
#ifndef ADDCONT_DB
#error ADDCONT_DB required
#endif
static void sql(sqlite3 *db, const char *command) {
    char *error = NULL;
    int rc = sqlite3_exec(db, command, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "%s: %s\n", command, error ? error : "failed");
    assert(rc == SQLITE_OK);
    sqlite3_free(error);
}
int main(void) {
    sqlite3 *db = NULL;
    assert(sqlite3_open(ADDCONT_DB, &db) == SQLITE_OK);
    sql(db, "CREATE TABLE tbl_addcontinfo (contentId TEXT PRIMARY KEY, state INTEGER)");
    sql(db, "INSERT INTO tbl_addcontinfo VALUES ('UP0001-CUSA10000_00-ADDON00000000001', 1)");
    sqlite3_close(db);
    const char *ids[2] = {"UP0001-CUSA10000_00-ADDON00000000001", "UP0001-CUSA10000_00-ADDON00000000002"};
    int states[2];
    db_dlc_registration(ids, 2, states);
    assert(states[0] == 1 && states[1] == 0);
    assert(sqlite3_open(ADDCONT_DB, &db) == SQLITE_OK);
    sql(db, "DROP TABLE tbl_addcontinfo");
    sql(db, "CREATE TABLE unknown_table (other TEXT)");
    sqlite3_close(db);
    db_dlc_registration(ids, 2, states);
    assert(states[0] == -1 && states[1] == -1);
    assert(sqlite3_open(ADDCONT_DB, &db) == SQLITE_OK);
    sql(db, "DROP TABLE unknown_table");
    sql(db, "CREATE TABLE tbl_records (content_id TEXT)");
    sql(db, "INSERT INTO tbl_records VALUES ('UP0001-CUSA10000_00-ADDON00000000002')");
    sqlite3_close(db);
    db_dlc_registration(ids, 2, states);
    assert(states[0] == 0 && states[1] == 1);
    remove(ADDCONT_DB);
    puts("DLC read-only DB checks: registered, absent, and unknown schemas OK");
    return 0;
}
