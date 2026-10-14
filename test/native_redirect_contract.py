#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).resolve().parent.parent
m=(r/'src/main.c').read_text(); w=(r/'src/web.c').read_text(); u=(r/'web/index.html').read_text()
s=m[m.index('/* Reversible, SEQUENTIAL experiment for 17'):m.index('void app_progress_json(buf_t *out)')]
assert '#define REDIRECT_COUNT 17' in s
assert s.count('"BH7') >= 17
assert 'pthread_create(&worker, &attr, redirect_all_worker, NULL)' in s
assert 'pthread_attr_setstacksize(&attr, 512 * 1024)' in s
assert 'unsigned char x[65536], y[65536];' not in s
assert 'unsigned char *both = malloc(chunk * 2)' in s
assert 'title_src_t *t = NULL;' in s and 'usb = calloc(REDIRECT_COUNT, sizeof(*usb))' in s
assert 'WORKER START sequential test' in s and 'WORKER EXIT status=' in s
assert 'STEP %d/%d %s: VERIFY START' in s and 'STEP %d/%d %s: STAGING + MOUNT START' in s
assert 'redirect_same_file_bytes(pkg, usb[i])' in s
assert 'memcmp(x, y, (size_t)n)' in s
assert 'db_dlc_registration(ids, REDIRECT_COUNT, db)' in s and 'db[i] != 1' in s
assert s.index('db_dlc_registration(ids, REDIRECT_COUNT, db)') > s.index('int app_redirect_native_dlc(')
assert s.index('redirect_same_file_bytes(pkg, usb[i])') < s.index('nullfs_mount(source, native)')
assert s.count('for (int i = 0; i < REDIRECT_COUNT; i++) {') >= 3
assert 'redirect_status(1, i+1)' in s
assert 'if (failed)' in s and 'ROLLBACK' in s and 'unmount(native, 0)' in s
assert 'top_layer(native, source) != 1 || owned_layer_count(native, source) != 1' in s
assert 'if (redirect_busy() || title_running(tid))' in s
assert 'unlink(native' not in s and 'rename(native' not in s
assert 'sceAppInstUtilAppInstallAll(' not in m
assert 'if (redirect_locked())' in m[m.index('int app_add('):m.index('int app_remove(')]
assert 'post && !strncmp(path, "/api/test-dlc-usb?", 18)' in w
assert 'post && !strncmp(path, "/api/restore-dlc-native?", 24)' in w
assert 'Testar 17 DLCs no USB' in u and 'Restaurar 17 DLCs no SSD' in u
assert 'Instalar DLCs (NATIVO / SSD)' in u
assert 'redirect_running' in m and 'redirect_stage' in u and 'redirect_current' in m and 'redirect_current' in u
print('17-DLC sequential verify+mount with heap buffers and rollback contracts OK')
