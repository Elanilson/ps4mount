#!/bin/bash
# Builds and runs the unit tests on the host (no PS5 or SDK needed): bash test/run.sh
set -e
cd "$(dirname "$0")/.."
mkdir -p build/test
# Generate templates with Python so host tests do not require xxd.
python3 - <<'PYGEN' > build/test/pdb_template.h
from pathlib import Path
for file, name in [('pdb_app_template.bin', 'k_pdb_template'),
                   ('pdb_patch_template.bin', 'k_pdb_patch_template'),
                   ('pdb_addcont_template.bin', 'k_pdb_addcont_template')]:
    data = (Path('tools') / file).read_bytes()
    print(f'static const unsigned char {name}[] = {{')
    for i in range(0, len(data), 16):
        print('  ' + ', '.join(f'0x{x:02x}' for x in data[i:i+16]) + ',')
    print('};')
    print(f'static const unsigned int {name}_len = {len(data)};')
PYGEN
${CC:-cc} -O1 -g -Wall -Wextra -Ibuild/test -Isrc -o build/test/unit test/unit.c src/build.c src/crypto.c src/pkg.c
build/test/unit

${CC:-cc} -O1 -g -Wall -Wextra -Werror -D_DEFAULT_SOURCE -Isrc -o build/test/scan_walk test/scan_walk.c src/scan_walk.c
build/test/scan_walk

python3 test/manual_contract.py

# Validate browser panel interactions without requiring a browser or PS5.
if command -v node >/dev/null 2>&1; then
    node test/panel_ui.cjs
fi

# Read-only addcont.db schema probing (synthetic host database).
${CC:-cc} -O0 -D_DEFAULT_SOURCE -DSQLITE_THREADSAFE=0 \
  '-DADDCONT_DB="build/test/addcont-fixture.db"' \
  -Isrc -Ithird_party/sqlite -o build/test/dlc_registry \
  test/dlc_registry.c src/db.c third_party/sqlite/sqlite3.c -lm -ldl -lpthread
build/test/dlc_registry
python3 test/dlc_bundle_contract.py

# Verify the no-copy USB revalidation path and read-only registry checks.
python3 test/usb_dlc_register_contract.py

# Regression test: a failed system delete must not suppress later USB DLC mounts.
${CC:-cc} -std=c11 -Wall -Wextra -Werror -Isrc -o build/test/uninstall_event test/uninstall_event.c
build/test/uninstall_event
python3 test/all_dlcs_contract.py
