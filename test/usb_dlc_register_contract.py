#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).resolve().parent.parent
s=(r/'src/main.c').read_text(); w=(r/'src/web.c').read_text(); ui=(r/'web/index.html').read_text()
checks={
 'no global installer': 'sceAppInstUtilAppInstallAll(' not in s,
 'targeted title refresh': 'single(tid, "/user/app/", NULL)' in s,
 'mount performs probe': 'usb_dlc_revalidate(&t, "MOUNT")' in s,
 'manual button performs probe': 'usb_dlc_revalidate(&t, "BUTTON")' in s,
 'database before and after': 'db_dlc_registration(ids, t->ndlc, before)' in s and 'db_dlc_registration(ids, t->ndlc, after)' in s,
 'explicit route': 'post && !strncmp(path, "/api/register-dlcs"' in w,
 'native installer separate': '/api/install-dlcs?' in w and 'app_install_dlcs' in s,
 'button renamed': 'Registrar DLCs (USB)' in ui and 'data-a="register-dlcs"' in ui,
 'native install only on explicit request': 'dlc_native_install_one' in s and 'if (!g_deleting && g_dlc_install_request[0]) install_dlcs_queued();' in s,
 'native internal addons are not overwritten': 'addcont_installed(t->dlc_tids[i])' in s,
 'error if DB does not confirm': 'return (missing || unknown) ? -1 : 0;' in s,
}
for k,v in checks.items(): print(('OK' if v else 'FAIL'),k)
assert all(checks.values())
