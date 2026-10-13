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

# Browser-only deterministic test of max-3 sequential writes and abort-on-first-error.
node tests/test_phase4f_panel.cjs
node tests/test_phase4h_batch.cjs

python3 tests/test_autoboot_source.py

# Phase 4K: audits and registration planning have no automatic writes.
node tests/test_phase4k_ui.cjs

# Phase 4L: upstream 0.4.0 config input hardening, settings UI and DLC metadata
# are read-only; no add-on installation or mass registration allowed.
cc -std=gnu11 -O1 -Wall -Wextra -Werror -Isrc \
  tests/test_upstream_safe.c src/upstream_safe.c -o /tmp/ps4mount-upstream-safe-test
/tmp/ps4mount-upstream-safe-test
node tests/test_phase4l_merge.cjs
