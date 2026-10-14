#!/usr/bin/env python3
"""Static integration tests. No PS5 emulator; cannot prove runtime semantics."""
from pathlib import Path
p=Path(__file__).resolve().parent.parent
s=(p/'src/main.c').read_text(); u=(p/'web/index.html').read_text(); w=(p/'src/web.c').read_text()
checks={
 'exactly mount/unmount buttons': u.count('data-a="mount"')==1 and u.count('data-a="unmount"')==1 and 'data-a="probe-' not in u,
 'no M1-M5 methods': 'dlc_probe_target(' not in s and '/api/probe-dlc?' not in w,
 'native installer per package': 'sceAppInstUtilInstallByPackage(' in s and 'sceAppInstUtilGetInstallStatus(' in s,
 'never AppInstallAll': 'sceAppInstUtilAppInstallAll(' not in s,
 'install queued by mount': 'stage 1/2 base mounted' in s and 'snprintf(g_dlc_install_request' in s,
 'DB must confirm native DLC': 'db_dlc_registration(&id,1,&state)' in s and 'db_dlc_registration(&cid,1,&registered)' in s and 'registered==1' in s,
 '0x80a30003 retry without treating success': '0x80a30003U' in s and 'untracked_retries' in s and 'return 1;' in s,
 'monitor native size and DB before offload': 'offload_wait_native_ready(&t.dlc[i], tid, installer_status_unavailable, force_reinstall)' in s and 'stable>=15' in s and 'current.st_size==usb.st_size' in s,
 'offload follows DB check': s.index('rc = offload_one(&t.dlc[i],tid)') > s.index('db_dlc_registration(&id,1,&state)'),
 'registered DLC never calls installer': 'rc = offload_reuse_registered(&t.dlc[i], tid)' in s and 'state == 0' in s and 'rc = dlc_native_install_one(&t.dlc[i]);' in s,
 'compare BEFORE any rename/unlink': s.index('redirect_same_file_bytes(pkg,rec.usb)') < s.index('if(rename(pkg,held))'),
 'journal BEFORE SSD mutation': s.index('if (offload_write(&rec))') < s.index('if(rename(pkg,held))'),
 'empty native file': 'open(pkg,O_CREAT|O_EXCL|O_WRONLY,0600)' in s,
 'mount validated BEFORE huge file unlink': s.index('if(offload_confirm_overlay(&rec))goto rollback;') < s.index('if(unlink(held))'),
 'survives restart': 'offload_resume_all();' in s,
 'unmount keeps PS5 registrations': 'release_title_base_patch(&t)' in s and 'uninstall_title_one(tid)' not in s[s.index('int app_remove('):s.index('static int active_status(',s.index('int app_remove('))],
 'manual DLC button calls separate force API': 'app_force_reinstall_dlcs' in s and '/api/reinstall-dlcs?' in w and 'data-a=\"reinstall-dlcs\"' in u,
 'forced install must await new native PKG': '!require_new_native_pkg && top_layer' in s and 'force_reinstall)' in s,
 'usb original PKGs untouched': 'unlink(rec.usb)' not in s,
 'read-only addcont DB': 'db_dlc_registration(' in s and 'sqlite3_exec(db, "UPDATE' not in (p/'src/db.c').read_text(),
}
for name,ok in checks.items():print(('OK' if ok else 'FAIL'),name)
assert all(checks.values())
print('Static one-click offload contracts passed (hardware untested)')
