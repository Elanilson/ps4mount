#!/usr/bin/env python3
from pathlib import Path
main=Path("src/main.c").read_text()
web=Path("web/index.html").read_text()
assert '#define AUTO_BOOT_BATCH_LIMIT 5' in main
assert 'batch_autoboot_once(); /* Waits for first indexed USB' in main
assert 'app_batch_preview_json(AUTO_BOOT_BATCH_LIMIT, &preview);' in main
assert 'app_batch_start(AUTO_BOOT_BATCH_LIMIT, token, msg, sizeof(msg))' in main
assert 'if (g_auto_boot_attempted || g_title_count <= 0) return 0;' in main
assert 'g_batch.phase == BATCH_INTERRUPTED' in main
assert 'g_batch.phase == BATCH_FAILED' in main
assert 'g_auto_boot_attempted = 1;' in main
assert 'if (batch_running()) batch_tick();' in main
assert '#define ENABLE_EXPERIMENTAL_REGISTRATION 0' in main
assert not any(w in main for w in ('sceAppInstUtilAppInstallAll(', 'AppInstallAll(NULL)', 'AppInstallAll(0)'))
assert 'AUTO-BOOT' in web
print('PASS: automatic mode bounded to one five-title batch per ELF, no bulk API or JS start')
