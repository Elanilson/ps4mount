#!/usr/bin/env python3
"""Static checks only: confirm that soft unmount cannot cascade DLC uninstalls."""
from pathlib import Path
s=(Path(__file__).resolve().parent.parent/'src/main.c').read_text()
ui=(Path(__file__).resolve().parent.parent/'web/index.html').read_text()
a=s[s.index('int app_remove('):s.index('static int active_status(',s.index('int app_remove('))]
r=s[s.index('static int release_base_patch_only('):s.index('int app_remove(')]
assert 'release_base_patch_only(tid)' in a
assert 'release_owned_layer(tid,"patch")' in r and 'release_owned_layer(tid,"app")' in r
assert '"addcont"' not in r, 'Soft disable must never release add-on layers'
assert 'sceAppInstUtilAppUnInstall(' not in s
assert 'offload_unmount_title(' not in s
assert 'offload_resume_title(tids[i])' in s
assert 'active_status(tids[i]) &&' not in s
assert 'if (needs_native_installer)' in s
assert 'no installer and no add-on unmount' in s
assert 'O icone pode continuar na biblioteca' in ui
assert 'game remains registered in PS5 library' in s
print('OK: DLC registration + native overlay + restart preserved by soft unmount (source contract)')
