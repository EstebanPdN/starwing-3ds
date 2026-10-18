#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BUILD="${STARWING_NATIVE_AUDIO_BUILD_DIR:-${ROOT}/build/native-audio-tests}"
cmake -S "${ROOT}" -B "${TEST_BUILD}" -DCMAKE_BUILD_TYPE=Release \
  -DSTARFOX_BUILD_RUNTIME=OFF -DSTARFOX_BUILD_TESTS=OFF \
  -DSTARFOX_BUILD_TOOLS=OFF -DSTARFOX_ENABLE_XBRZ=OFF "$@"
cmake --build "${TEST_BUILD}" --target snes_spc_core --parallel 2
"${CXX:-c++}" -std=c++20 -O2 -DNDEBUG -Wall -Wextra \
  -I"${TEST_BUILD}/generated/snes_spc" "${ROOT}/tests/native_spc_block_test.cpp" \
  "${TEST_BUILD}/libsnes_spc_core.a" -o "${TEST_BUILD}/native_spc_block_test"
"${TEST_BUILD}/native_spc_block_test"
