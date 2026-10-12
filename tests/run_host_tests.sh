#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -Wno-unused-function -Wno-unused-variable -Wno-maybe-uninitialized \
  -ffunction-sections -fdata-sections -Itests/host_include \
  -Wl,--gc-sections tests/test_nested_scan.c -o /tmp/ps4mount-nested-test
/tmp/ps4mount-nested-test
