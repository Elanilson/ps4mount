#!/usr/bin/env python3
from pathlib import Path
r=Path(__file__).resolve().parent.parent
m=(r/'src/main.c').read_text();w=(r/'src/web.c').read_text();u=(r/'web/index.html').read_text(); h=(r/'src/web.h').read_text()
s=m[m.index('/* v0.4.18: single GoW DLC reversible experiment.') : m.index('void app_progress_json(buf_t *out)')]
assert '#define PILOT_TID "CUSA34384"' in s
assert 'static int pilot_read(pilot_record_t *r)' in s
assert 'char line[PATH_MAX+64];' in s and 'memcpy(v[i],line,n+1)' in s
assert s.index('pilot_write(r)') < s.index('rename(pkg,held)') < s.index('nullfs_mount(r->source,r->native)')
assert 'redirect_same_file_bytes(pkg,r->usb)' in s
assert 'redirect_copy_meta(r->native,r->source,meta[i])' in s
assert 'pthread_attr_setstacksize(&attr,512*1024)' in s
assert 'rename(held,pkg)' in s and 'unmount(r.native,0)' in s
assert 'unlink(held)' not in s and 'unlink(pkg)' not in s
assert 'db_dlc_registration(ids,1,states)' in s and 'states[0]!=1' in s
assert 'app_pilot_restore_current' in s and 'pilot_restore_internal(1)' in m
assert 'fsync(fd)' in s and 'PILOT_JOURNAL' in s
assert 'only_cid[0] && strcmp(t.dlc[i].pkg.content_id, only_cid)' in m
assert 'int todo = only_cid[0] ? 1 : t.ndlc;' in m
assert '[dlc-pilot] SINGLE NATIVE INSTALL queued' in m
for endp in ('pilot-install','pilot-redirect','pilot-restore','pilot-dlcs'):
    assert '/api/'+endp+'?' in w
for btn in ('Instalar 1 DLC (NATIVO)','Testar 1 DLC (USB)','Restaurar 1 DLC (SSD)'):
    assert btn in u
assert 'app_pilot_restore_current' in h
assert 'sceAppInstUtilAppInstallAll(' not in m
assert 'PILOT_HELD_SUFFIX' in s and 'NO native ac.pkg' in s
print('GoW single-DLC pilot: journal, selective install, compare, reversible overlay, restore contracts OK')
