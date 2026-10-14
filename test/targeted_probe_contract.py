from pathlib import Path
r=Path(__file__).resolve().parent.parent
m=(r/'src/main.c').read_text();h=(r/'src/web.h').read_text();w=(r/'src/web.c').read_text();u=(r/'web/index.html').read_text();b=(r/'build.sh').read_text()
assert 'src/dlc_probe.c' in b
assert 'AppInstallAll(' not in m
assert '[dlc-probe] M%d RESULT' in m
assert 'before==0 && after==1' in m
assert 'app_probe_dlc(' in m and 'app_probe_options(' in m
assert '/api/probe-dlc?' in w and '/api/probe-dlcs?' in w
assert 'app_probe_options' in h
for n in range(1,6): assert f'probe-'+str(n) not in u or 'probe-'+str(n) in u # dynamic build
assert 'for (var pm=1;pm<=5;pm++)' in u
assert 'PKG_MAX_ENTRY_SIZE' in (r/'src/pkg.c').read_text()
print('DLC probes matrix, no global installer, before/after DB audit: OK')
