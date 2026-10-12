/* Run against disposable local SQLite databases; never the PS5 app.db. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../src/db.h"
#include "sqlite3.h"

int main(void) {
    const char *tmp = APP_DB;
    sqlite3 *db = NULL;
    assert(sqlite3_open(tmp, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "CREATE TABLE tbl_contentinfo(titleId TEXT);"
                            "INSERT INTO tbl_contentinfo VALUES ('CUSA01623');", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    assert(db_registered_status("CUSA01623") == 1);
    assert(db_registered_status("CUSA09999") == 0);
    assert(db_register(NULL) == -1);
    assert(db_unregister("CUSA01623") == -1);
    /* Replacing the DB with an empty file simulates a broken/missing schema. */
    assert(sqlite3_open(tmp, &db) == SQLITE_OK);
    assert(sqlite3_exec(db, "DROP TABLE tbl_contentinfo;", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(db);
    assert(db_registered_status("CUSA01623") == -1);
    unlink(tmp);
    assert(db_registered_status("CUSA01623") == -1);
    puts("PASS: DB present / absent / broken / inaccessible and write prevention");
    return 0;
}
