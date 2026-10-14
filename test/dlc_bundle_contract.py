#!/usr/bin/env python3
"""Static mount/unmount contract; not PS5 hardware validation."""
from pathlib import Path
s = (Path(__file__).resolve().parent.parent/'src/main.c').read_text()
m = s[s.index('static int mount_paths('):s.index('static int mount_title(')]
u = s[s.index('static int release_title_bundle('):s.index('int app_remove(')]
r = s[s.index('int app_remove('):s.index('static int active_status(', s.index('int app_remove('))]
assert 'const char *kind = i == 0 ? "app" : (i == 1 ? "patch" : "addcont")' in m
assert 'goto rollback;' in m and 'acquired[j].added' in m
assert 'release_owned_layer(tid, "patch")' in u and 'release_owned_layer(tid, "app")' in u
assert 'release_owned_layer(id, "addcont")' not in u
assert 'release_owned_layer(' not in r  # only through base/patch helper
assert 'offload_unmount_title(tid)' not in r
assert 'release_title_bundle(&t)' in r
assert 'uninstall_title_one(tid)' in r  # preserve v0.4.21 system targeted uninstall
assert 'sceAppInstUtilAppInstallAll(' not in s
assert '#define MAX_DLC_TIDS MAX_DLC' in s
assert 'db_dlc_registration(ids, t->ndlc, states)' in s
print('DLC bundle source contracts: OK (app/patch only; targeted uninstall unchanged)')
