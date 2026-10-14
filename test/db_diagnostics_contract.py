from pathlib import Path
m = Path('src/main.c').read_text()
w = Path('src/web.c').read_text()
ui = Path('web/index.html').read_text()
db = Path('src/db.c').read_text()
assert 'int app_diagnose_db(' in m
assert 'db_log_addcont_title(tid)' in m
assert 'post && !strncmp(path, "/api/diagnose-db?", 17)' in w
assert 'app_diagnose_db' in m
assert 'mode=ro&nolock=1' in db
assert 'SQLITE_OPEN_READONLY | SQLITE_OPEN_URI' in db
assert 'WHERE instr(' in db and 'sqlite3_bind_text(q, 1, title_id' in db
assert 'diagnostic_col_safe' in db
assert 'sqlite3_exec(db, "UPDATE' not in db
print('DB inspection: POST-only, CUSA-scoped, read-only, safe fields OK')
