#!/usr/bin/env python3
"""Source-level safety checks, not a substitute for PS5 hardware integration tests."""
from pathlib import Path
root=Path(__file__).resolve().parent.parent
main=(root/'src/main.c').read_text()
web=(root/'src/web.c').read_text()
page=(root/'web/index.html').read_text()
db=(root/'src/db.c').read_text()
checks={
 'global AppInstallAll is not callable': 'sceAppInstUtilAppInstallAll(' not in main,
 'boot restores persisted titles': 'restore_active_games();' in main,
 'boot never calls recursive scan': 'scan_all();' not in main and 'scan_root(' not in main,
 'manual USB discovery has a dedicated flag': 'g_discover_requested' in main and 'adopt_root_pkgs(root)' in main,
 'manual activation queues one CUSA': 'g_activate_request[10]' in main and 'single(tid, "/user/app/", NULL)' in main,
 'no implicit global installer fallback': 'kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE")' in main,
 'restore never repairs state automatically': 'No automatic repair' in main,
 'no USB preflight before restoration': 'if (!any_external_usb())' in main and 'SKIPPED: no mounted external USB' in main,
 'already-registered game mounts without old ownership proof': 'mount_registered_title(&t)' in main and 'marker_owned_folder(t)' not in main,
 'registered game can refresh USB metadata without an installer': 'existing DB registration, rebuilding USB metadata (no installer)' in main and 'place_files(t)' in main,
 'manual search emits live folder and package counters': 'g_scan_folders++' in main and 'g_scan_examined++' in main and 'g_scan_organized++' in main,
 'manual search can return live progress during traversal': ('walk_pkg_dirs_progress(' in main or 'walk_pkg_dirs_progress_cancel(' in main) and 'pump_events(1)' in main,
 'indeterminate panel progress while search total unknown': "removeAttribute('value')" in page and 'p.scan_folders' in page,
 'mount/unmount button wording': '>Montar</button>' in page and '>Desmontar</button>' in page,
 'mounted state wins over inactive marker': main.index('else if (mounted) state = "ready"') < main.index('else if (removed(tid) || !active_status(tid)) state'),
 'failed writes leave an attempt guard': 'STATE_DIR "/%s.attempt"' in main and 'exists(attempt)' in main,
 'app.db freshness checked before staging': 'db_snapshot_healthy()' in main and 'g_last_good' in db,
 'no forced mount teardown': 'MNT_FORCE' not in main and 'unmount_all_layers(' not in main,
 'foreign mounts are rejected': 'target_mounts_safe(&t)' in main and 'top_layer(target, dir)' in main,
 'unmount retains library registration': 'release_title_base_patch(&t)' in main and 'uninstall_title_one(tid)' not in main[main.index('int app_remove('):main.index('static int active_status(',main.index('int app_remove('))],
 'progress GET is read only': 'app_progress_json(&b)' in web and '"/api/progress") && !post' in web,
 'explicit POST search': 'post && !strcmp(path, "/api/rescan")' in web,
 'search and mount buttons present': 'Buscar jogos no USB' in page and '>Montar</button>' in page,
 'minimal notification cadence': 'restoring %d previously active games' in main and 'restore complete:' in main,
 'DLC staging groups logged': '[manual][dlc]' in main,
}
for name,good in checks.items():
    print(('OK' if good else 'FAIL'),name)
assert all(checks.values()), 'manual restore contract broken'
print(len(checks), 'manual contract checks passed')
