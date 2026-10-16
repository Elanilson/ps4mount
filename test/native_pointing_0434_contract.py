#!/usr/bin/env python3
"""v0.4.34 static contracts; cannot simulate PS5 kernel mounts."""
from pathlib import Path
r = Path(__file__).resolve().parent.parent
s = (r/'src/main.c').read_text(); u = (r/'web/index.html').read_text()

def between(start, end):
    return s[s.index(start):s.index(end, s.index(start))]
clean = between('int app_clean_dlcs(', 'void app_games_json(')
clean_worker = between('static int clean_native_title(', '/* Reversible, SEQUENTIAL experiment')
manual = between('static void install_dlcs_queued(void)', '/* Native-only 0-byte cleanup')
point = manual[manual.index('if (manual_pointing) {\n            /* No DB read'): manual.index('} else if (state == 1')]
mount = between('static int mount_registered_title(', 'static void activate_queued(')
assert 'installed_internally(tid) || title_running(tid)' not in between('int app_add(', 'static volatile int g_one_uninstall_busy')
assert 'installed_internally(tid)' not in between('static int title_state(', 'static void pump_events(int ms);')
assert 'native_game_open(tid,"app",&nat_app)' in mount
assert mount.index('mount_title(t)') < mount.index('native_game_zero(&nat_app,tid,"app")')
assert 'ftruncate(p->fd,0)' in s and 'fstatfs(fd,&sf)' in s
assert 'if (!manual_pointing) db_dlc_registration(ids, t.ndlc, states)' in manual
assert 'if (!manual_pointing) db_dlc_registration(&id, 1, &state)' in manual
assert 'db_dlc_registration(' not in point
assert 'offload_one(&t.dlc[i],tid,1)' in point
assert 'CREATED native empty ac.pkg' in s
assert 'build_app_json(&json,&dlc->pkg,dlc->path)' in s
assert 'g_dlc_manual_pointing=1;' in s and 'queued manual pointing for' in s
assert 'db_is_registered' not in clean and 'db_dlc_registration' not in clean
assert 'ftruncate(fd,0)' in clean_worker and 'O_NOFOLLOW' in clean_worker
assert 'g_native_dlc_roots[storage]' in clean_worker
assert 'unlink(rec.usb)' not in s and 'rename(rec.usb' not in s and 'ftruncate(rec.usb' not in s
assert "'internal'].indexOf(g.state) >= 0" in u
print('v0.4.34 native-only cleanup / no-DB pointer / installed game mount contracts passed')
