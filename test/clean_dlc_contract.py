#!/usr/bin/env python3
"""v0.4.33: bulk cleanup is physical PS5-storage-only and independent of USB."""
from pathlib import Path
p=Path(__file__).resolve().parent.parent
s=(p/'src/main.c').read_text();w=(p/'src/web.c').read_text();u=(p/'web/index.html').read_text()
a=s[s.index('int app_clean_dlcs('):s.index('void app_games_json',s.index('int app_clean_dlcs('))]
step=s[s.index('/* Native-only 0-byte cleanup'):s.index('/* Reversible, SEQUENTIAL experiment',s.index('/* Native-only 0-byte cleanup'))]
one=s[s.rindex('static int offload_one('):s.rindex('static int offload_reuse_registered(')]
manual=s[s.index('static void install_dlcs_queued(void) {'):s.index('/* Native-only 0-byte cleanup',s.index('static void install_dlcs_queued(void) {'))]
assert 'g_native_dlc_roots' in a and 'catalog_refresh()' not in a and 'any_external_usb()' not in a
assert 'strlen(tid)!=9' not in a and 'strncmp(tid,"CUSA",4)' not in a
assert 'db_snapshot_healthy()' not in a and 'db_is_registered(' not in a
assert 'g_native_dlc_roots[storage]' in step and 'strcmp(name,"ac.pkg")' in step
assert 'strlen(label)!=16' not in step and 'if (strlen(label)' not in step
assert 'ftruncate(fd,0)' in step
assert 'O_NOFOLLOW' in step and 'fstatfs(fd,&filefs)' in step
assert 'native_clean_safe_fs' in step
assert 'layers_on(dir)' in step and 'layers_on(path)' in step
assert 'stat(rec.usb' not in step and 'unlink(rec.usb' not in step
assert 'offload_one(' not in step and 'install_dlcs_queued()' not in step
assert 'source.st_size<=0' in one and 'matched.st_size<=0' not in one[one.index('if (fast_after_install) {'):one.index('} else {',one.index('if (fast_after_install) {'))]
assert 'native ac.pkg already 0B' in one
assert 'if(moved && unlink(held))' in one
assert '(!manual_pointing && (!db_snapshot_healthy() || !db_is_registered(tid)))' in manual
assert 'Limpar DLCs' in u and '/api/clean-dlcs' in u and '/api/clean-dlcs' in w
assert "if (!confirm('CONFIRMACAO FINAL:" in u
print('Native-only destructive cleanup contract: OK')
