#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BUILD="${STARWING_HOST_TEST_BUILD_DIR:-${ROOT}/build/host-tests}"
mkdir -p "${TEST_BUILD}"
for TEST in frame_pacing launch_selection screen_transfer; do
  "${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra \
    "${ROOT}/tests/${TEST}_test.cpp" -o "${TEST_BUILD}/${TEST}_test"
  "${TEST_BUILD}/${TEST}_test"
  printf 'PASS: %s\n' "${TEST}"
done
python3 "${ROOT}/platform/3ds/tools/test_analyze_hardware_dump.py"
