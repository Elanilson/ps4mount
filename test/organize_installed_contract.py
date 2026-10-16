#!/usr/bin/env python3
"""Check that organizer does not use the PS5 internal-install check."""
from pathlib import Path
p=Path(__file__).resolve().parent.parent/'src/main.c'
s=p.read_text()
organize=s[s.index('static int adopt_dir_pkgs('):s.index('typedef struct {\n    char tid[10], name[128];', s.index('static int adopt_dir_pkgs('))]
assert 'if (installed_internally(tid))' not in organize, 'internal install blocks USB PKG organization'
assert 'PS4Mount 0.4.39-organize-installed-usb-pkgs-TEST' in s
assert 'rename(path, dst)' in organize
assert 'adopt_waiting_dlcs(root)' in s
assert s.index('adopt_waiting_dlcs(root)') < s.index('int still_waiting = waiting_count(root)')
assert 'installed_internally(tid)' in s, 'preserve internal check for mount/registration'
assert 'find_dlc_game(root, pkg, game)' in s
print('OK: installed game does not block discovery; waiting DLCs retried on same drive')
