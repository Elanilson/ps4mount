#!/usr/bin/env python3
"""Structural regression checks for experimental manual-only offload."""
from pathlib import Path
r=Path(__file__).resolve().parent.parent
main=(r/'src/main.c').read_text()
web=(r/'src/web.c').read_text()
ui=(r/'web/index.html').read_text()
start=main.index('static void install_dlcs_queued(void) {')
end=main.index('/* Reversible, SEQUENTIAL experiment',start)
worker=main[start:end]
pnt=worker[worker.index('if (manual_pointing) {'):worker.index('} else if (state == 1 && !force_reinstall)')]
native=worker[worker.index('} else if (state == 0 || (state == 1 && force_reinstall))'):worker.index('} else {\n            printf("[dlc-install] %s DB state unknown')]
assert 'offload_one(&t.dlc[i],tid,1)' in pnt
assert 'dlc_native_install_one' not in pnt
assert 'offload_native_ready_fast' not in pnt
assert 'offload_one(' not in native
assert 'offload_native_ready_fast' in native # limited scheduling probe only
assert '!manual_pointing && i < t.ndlc_tids' in worker
assert 'db_dlc_registration(&id, 1, &state)' in worker
assert 'g_dlc_manual_pointing = 1' in main
assert 'queue_dlc_job(tid, msg, cap, 0)' in main
assert 'api/point-dlcs?' in web and 'app_manual_point_dlcs(' in web
assert 'data-a="point-dlcs"' in ui
assert 'ATENCAO:' in ui and 'TERMINARAM no PS5' in ui
assert main.index('if (offload_write(&rec))') < main.index('if(rename(pkg,held))')
assert main.index('if(offload_confirm_overlay(&rec))goto rollback;') < main.index('if(unlink(held))')
assert 'SCE_OK' not in pnt # must not misread accepted request as completion
print('Manual pointing contracts passed')
