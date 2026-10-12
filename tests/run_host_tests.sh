#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-function -Wno-unused-variable -Wno-maybe-uninitialized \
  -ffunction-sections -fdata-sections -Itests/host_include \
  -Wl,--gc-sections tests/test_nested_scan.c src/organize.c -o /tmp/ps4mount-nested-test

cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-function -Wno-unused-variable -Wno-maybe-uninitialized \
  -ffunction-sections -fdata-sections -Itests/host_include \
  -Wl,--gc-sections tests/test_mount_guards.c -o /tmp/ps4mount-mount-test
/tmp/ps4mount-mount-test
/tmp/ps4mount-nested-test


# Test read-only SQLite queries against a throwaway host database.
DB_PATH=$(mktemp /tmp/ps4mount-db-target-XXXXXX)
trap 'rm -f "$DB_PATH"' EXIT
cc -std=gnu11 -O1 -Wall -Wextra -Ithird_party/sqlite \
  -DAPP_DB=\"$DB_PATH\" tests/test_db_status.c src/db.c -lsqlite3 -o /tmp/ps4mount-db-test
/tmp/ps4mount-db-test

# Defensive parser checks against malformed PKG/SFO metadata.
cc -std=gnu11 -O1 -Wall -Wextra -I./src \
  tests/test_sfo_bounds.c src/pkg.c src/crypto.c -o /tmp/ps4mount-sfo-test
/tmp/ps4mount-sfo-test

# Real temp-directory moves and explicit crash-recovery tests; NEVER PS5 paths.
cc -std=gnu11 -O1 -Wall -Wextra -Wno-format-truncation -Wno-unused-function \
  -ffunction-sections -fdata-sections -I./src tests/test_organize.c -o /tmp/ps4mount-organize-test
/tmp/ps4mount-organize-test
