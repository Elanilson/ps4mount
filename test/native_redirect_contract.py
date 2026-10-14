#!/usr/bin/env python3
from pathlib import Path
r = Path(__file__).resolve().parent.parent
m = (r/'src/main.c').read_text()
w = (r/'src/web.c').read_text()
u = (r/'web/index.html').read_text()
s = m[m.index('/* One-item reversible experiment'):m.index('void app_progress_json(buf_t *out)')]
assert 'CUSA03842' in s and 'BH70000000DLC001' in s
assert 'REDIRECT_TARGET "/ac.pkg"' in s and 'S_ISREG(native.st_mode)' in s
assert 'redirect_same_file_bytes' in s and 'memcmp(x, y, (size_t)n)' in s
assert 'db_dlc_registration(&cid, 1, &db)' in s and 'db != 1' in s
assert 'nullfs_mount(REDIRECT_SOURCE, REDIRECT_TARGET)' in s
assert 'unmount(REDIRECT_TARGET, 0)' in s
assert 'S_ISREG(st.st_mode)' in s
assert 'unlink(REDIRECT_TARGET' not in s and 'rename(REDIRECT_TARGET' not in s
assert 'sceAppInstUtilAppInstallAll(' not in m
assert 'if (redirect_mounted())' in m[m.index('int app_install_dlcs('):m.index('static int dlc_installer_release_external_layer')]
assert 'post && !strncmp(path, "/api/test-dlc-usb?", 18)' in w
assert 'post && !strncmp(path, "/api/restore-dlc-native?", 24)' in w
assert 'Testar 1 DLC no USB' in u and 'Restaurar DLC no SSD' in u
assert 'Instalar DLCs (NATIVO / SSD)' in u
print('Pilot 1-DLC nullfs overlay, native restore and explicit install contracts OK')
