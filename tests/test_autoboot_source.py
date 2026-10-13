#!/usr/bin/env python3
from pathlib import Path
m=Path('src/main.c').read_text(); w=Path('src/web.c').read_text(); ui=Path('web/index.html').read_text()
assert '#define AUTO_BOOT_BATCH_LIMIT 0' in m
assert 'batch_autoboot_once();' in m
assert '#if AUTO_BOOT_BATCH_LIMIT > 0' in m
assert 'static void sync_tick(void)' in m
assert 'void app_sync_status_json(buf_t *out)' in m
assert 'if(changed)sync_begin();' in m
assert 'sync_tick();' in m
assert 'if(g_sync.processed%25==0 && g_sync.processed<g_sync.total)' in m
assert 'NO REGISTRATION' in m
assert '"/api/sync-status"' in w and 'app_sync_status_json(&b)' in w
assert 'Sincronizar (somente leitura)' in ui
assert "api('/api/sync-status')" in ui
assert "api('/api/rescan', 'POST')" in ui
assert 'id="batchpreview" class="primary"' in ui
assert 'id="groupprogress"' in ui
assert 'id="grouplive"' in ui
assert 'function batchRefresh()' in ui
assert "api('/api/batch-start?limit='" in ui
assert not any(x in m for x in ('sceAppInstUtilAppInstallAll(', 'AppInstallAll(NULL)', 'AppInstallAll(0)'))
print('PASS: read-only incremental sync and safe web refresh, no auto-registration')
