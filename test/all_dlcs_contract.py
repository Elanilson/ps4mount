#!/usr/bin/env python3
"""Behavioral source contracts for explicit mount and individual DLC install."""
from pathlib import Path
p=Path(__file__).resolve().parent.parent
s=(p/'src/main.c').read_text()
ui=(p/'web/index.html').read_text()
dlc_open=s[s.index('static void dlc_open('):s.index('static int first_time(')]
src_open=s[s.index('static int src_open('):s.index('static int src_open(')+4000]
install=s[s.index('static void register_dlcs_queued(void) {'):s.index('void app_progress_json(',s.index('static void register_dlcs_queued(void) {'))]
delete=s[s.index('static void handle_klog_line('):s.index('/* Serves the panel',s.index('static void handle_klog_line('))]
assert 'STATE_DIR "/%s.removed"' not in dlc_open and 'if (exists(rm))' not in dlc_open, 'DLC discovery must never gate on .removed'
assert 'dlc_open(t);' in src_open and 'if (g_cfg.dlc)' not in src_open
assert 'offload_one(&t.dlc[i],tid)' in install
assert 'sceAppInstUtilInstallByPackage(' in s and 'sceAppInstUtilAppInstallAll(' not in s
assert 'ps4mount_uninstall_result(p, &uninstall_rc)' in delete
assert 'if (uninstall_rc != 0)' in delete
assert 'STATE_DIR "/%s.removed", id' not in delete, 'DLC delete event must not create legacy removal markers'
assert 'rm_tree(hold);' not in delete[delete.index('} else if (strlen(id) == 36 && g_deleting)'):delete.index('/* the delete is done', delete.index('} else if (strlen(id) == 36 && g_deleting)'))]
assert 'data-a="mount"' in ui and 'data-a="unmount"' in ui
assert 'id="s-dlc"' not in ui
assert 'sceAppInstUtilAppInstallAll(' not in s
print('All-DLC mount/USB registration regression contracts: OK')
