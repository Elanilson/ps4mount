#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).resolve().parent.parent
s=(r/'src/main.c').read_text(); w=(r/'src/web.c').read_text(); ui=(r/'web/index.html').read_text()
checks={
 'no global or native installer': 'sceAppInstUtilAppInstallAll(' not in s and 'sceAppInstUtilInstallByPackage(' not in s,
 'targeted title refresh': 'single(tid, "/user/app/", NULL)' in s,
 'mount performs probe': 'usb_dlc_revalidate(&t, "MOUNT")' in s,
 'manual button performs probe': 'usb_dlc_revalidate(&t, "BUTTON")' in s,
 'database before and after': 'db_dlc_registration(ids, t->ndlc, before)' in s and 'db_dlc_registration(ids, t->ndlc, after)' in s,
 'explicit route': 'post && !strncmp(path, "/api/register-dlcs"' in w,
 'legacy native route removed': '/api/install-dlcs' not in w,
 'button renamed': 'Registrar DLCs (USB)' in ui and 'data-a="register-dlcs"' in ui,
 'no native install function': 'dlc_native_install_one' not in s,
 'native internal addons are not overwritten': 'addcont_installed(t->dlc_tids[i])' in s,
 'error if DB does not confirm': 'return (missing || unknown) ? -1 : 0;' in s,
}
for k,v in checks.items(): print(('OK' if v else 'FAIL'),k)
assert all(checks.values())
