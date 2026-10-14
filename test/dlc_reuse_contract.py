#!/usr/bin/env python3
"""Fast path and repair path checks (static, not hardware tests)."""
from pathlib import Path
s=(Path(__file__).resolve().parent.parent/'src/main.c').read_text()
q=s[s.index('static void install_dlcs_queued(void) {'):s.index('/* Reversible, SEQUENTIAL experiment',s.index('static void install_dlcs_queued(void) {'))]
h=s[s.index('static int offload_reuse_or_plan(const dlc_src_t *dlc,const char *tid) {'):s.index('/* Fallback for FW 11.40',s.index('static int offload_reuse_or_plan(const dlc_src_t *dlc,const char *tid) {'))]
assert 'offload_reuse_or_plan(&t.dlc[i],tid)' in q
assert q.index('offload_reuse_or_plan(&t.dlc[i],tid)') < q.index('dlc_native_install_one(&t.dlc[i])')
assert 'if (!rc && need_native_install)' in q
assert 'if (!rc && need_native_offload)' in q
assert 'offload_prepare_native_reinstall(&t.dlc[i])' in q
assert 'offload_wait_native_ready(&t.dlc[i], tid, installer_status_unavailable)' in q
assert q.index('if (!rc && need_native_install)') < q.index('dlc_installer_release_external_layer(dtid)')
assert q.index('dlc_installer_release_external_layer(dtid)') < q.index('dlc_native_install_one(&t.dlc[i])')
assert 'offload_one(dlc,tid) ? -1 : 0' in h
assert 'return 1; /* no journal, no native copy: reinstall to restore files */' in h
assert 'return 2; /* existing stable candidate; verify before offload */' in h
assert 'journal invalid or USB changed; no installer/no file deletion' in h
assert 'overlay exists without journal' in h
print('DLC reuse/repair contracts: OK')
