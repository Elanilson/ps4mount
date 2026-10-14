#!/usr/bin/env python3
"""Checks critical mount/unmount contracts in PS5-specific C; NOT a console test."""
from pathlib import Path
s = (Path(__file__).resolve().parent.parent/'src/main.c').read_text()
m = s[s.index('static int mount_paths('):s.index('static int mount_title(')]
u = s[s.index('static int release_base_patch_only('):s.index('int app_remove(')]
r = s[s.index('int app_remove('):s.index('static int active_status(', s.index('int app_remove('))]
assert 'const char *kind = i == 0 ? "app" : (i == 1 ? "patch" : "addcont")' in m
assert 'goto rollback;' in m and 'acquired[j].added' in m
assert 'release_owned_layer(tid,"patch")' in u
assert 'release_owned_layer(tid,"app")' in u
assert 'release_owned_layer(id, "addcont")' not in u
assert 'release_base_patch_only(tid)' in r
assert 'sceAppInstUtilAppUnInstall(' not in s
assert 'sceAppInstUtilAppInstallAll(' not in s
assert '#define MAX_DLC_TIDS MAX_DLC' in s
assert 'db_dlc_registration(ids, t->ndlc, states)' in s
print('DLC bundle source contracts: OK')
