from pathlib import Path
r=Path(__file__).resolve().parent.parent
s=(r/'src/main.c').read_text();ui=(r/'web/index.html').read_text()
checks={
 'one global API call':s.count('sceAppInstUtilAppInstallAll(NULL)')==1,
 'worker is asynchronous':'pthread_create(&th, NULL, original_global_worker, NULL)' in s,
 'no synchronous fallback':'NO synchronous fallback' in s,
 'no filtering already registered': 'PRE %s state=%d (NO FILTER)' in s,
 'no filtering title db in global work': '!db_is_registered(tid) || title_running(tid)' not in s[s.index('static void register_dlcs_queued(void) {'):s.index('void app_progress_json(buf_t *out) {')],
 'global is explicit':'Registrar DLCs (GLOBAL)' in ui and 'AppInstallAll e GLOBAL' in ui,
 'no global call on mount':'usb_dlc_revalidate(&t, "MOUNT")' not in s and s.count('sceAppInstUtilAppInstallAll(NULL)')==1,
 'native install absent':'sceAppInstUtilInstallByPackage(' not in s,
 'reads before and after':'db_dlc_registration(ids, t.ndlc, g_original_global_before)' in s and 'db_dlc_registration(ids, g_original_global_count, states)' in s,
 'stop deferred':'stop deferred until GLOBAL AppInstallAll finishes' in s,
}
for k,v in checks.items():print(('OK' if v else 'FAIL'),k)
assert all(checks.values())
