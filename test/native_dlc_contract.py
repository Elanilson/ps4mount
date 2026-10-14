#!/usr/bin/env python3
"""Source-level regression checks; native PS5 integration remains untested."""
from pathlib import Path
r=Path(__file__).resolve().parent.parent
s=(r/'src/main.c').read_text()
w=(r/'src/web.c').read_text()
ui=(r/'web/index.html').read_text()
checks={
 'no global installer': 'sceAppInstUtilAppInstallAll(' not in s,
 'explicit DLC queue': 'g_dlc_install_request' in s and 'install_dlcs_queued();' in s,
 'one individual package per loop': 'dlc_native_install_one(&t.dlc[i])' in s,
 'native package API': 'sceAppInstUtilInstallByPackage(&meta, &info, playgo)' in s,
 'native task status': 'sceAppInstUtilGetInstallStatus(cid, &status)' in s,
 'logs native return codes': 'request %s rc=0x%08x' in s,
 'checks addon database': 'db_dlc_registration(&id, 1, &st)' in s,
 'does not install on normal Mount': 'install_dlcs_queued();' in s[s.index('int main(void)'):],
 'explicit POST route': 'post && !strncmp(path, "/api/install-dlcs"' in w,
 'no GET install route': 'post && !strncmp(path, "/api/install-dlcs"' in w,
 'explicit game card button': 'data-a="install-dlcs"' in ui,
 'native progress in card': 'p.dlc_percent' in ui,
 'prevents simultaneous install and mount': '"installing_dlcs"' in s[s.index('int app_add('):s.index('int app_add(') + 1600],
 'unknown native task never remounts addon overlay': 'opened && !native_task_unknown && mount_title(&t)' in s,
 'internal installs not removed on Unmount': 'AppUnInstallAddcont' not in s and 'release_title_bundle' in s,
}
for name, ok in checks.items(): print(('OK' if ok else 'FAIL'),name)
assert all(checks.values())
print(len(checks),'DLC installer contract checks passed')
