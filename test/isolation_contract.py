from pathlib import Path
s=Path('src/main.c').read_text(); i=Path('src/isolation.c').read_text(); w=Path('web/index.html').read_text()
assert 'Isolar outros' in w and 'Restaurar outros' in w
assert 'app_isolate_others' in s and 'app_restore_others' in s
assert 'STARTUP_RECOVERY' in s
assert 'sceAppInstUtilAppInstallAll(NULL)' in s and s.count('sceAppInstUtilAppInstallAll(NULL)') == 1
assert 'HOLD_DIR' in s and 'rename(normal, hidden)' in i and 'rename(hidden, normal)' in i
assert 'mount(' not in i and 'unmount(' not in i and '/user/' not in i.replace('live /user', '')
assert 'isolation_operation_busy()' in s
assert 'isolation_status(HOLD_DIR' in s
print('OK: isolation contract, no mountpoint rename and no extra global API call')
