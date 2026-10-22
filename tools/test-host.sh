#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TEST_BUILD="${STARWING_HOST_TEST_BUILD_DIR:-${ROOT}/build/host-tests}"
mkdir -p "${TEST_BUILD}"
for TEST in top_viewport presentation_limit frame_pacing launch_selection screen_transfer presentation_metrics performance_history diagnostic_sha256 update_notes; do
  "${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra \
    "${ROOT}/tests/${TEST}_test.cpp" -o "${TEST_BUILD}/${TEST}_test"
  "${TEST_BUILD}/${TEST}_test"
  printf 'PASS: %s\n' "${TEST}"
done
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -I"${ROOT}/include" \
  "${ROOT}/tests/saturate_test.cpp" -o "${TEST_BUILD}/saturate_test"
"${TEST_BUILD}/saturate_test"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -I"${ROOT}/include" \
  "${ROOT}/tests/texel_expand_test.cpp" -o "${TEST_BUILD}/texel_expand_test"
"${TEST_BUILD}/texel_expand_test"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -I"${ROOT}/include" \
  "${ROOT}/tests/bg2_run_test.cpp" "${ROOT}/src/render/background_renderer.cpp" \
  -o "${TEST_BUILD}/bg2_run_test"
"${TEST_BUILD}/bg2_run_test"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -I"${ROOT}/include" \
  "${ROOT}/tests/character_cache_test.cpp" "${ROOT}/src/render/background_renderer.cpp" \
  -o "${TEST_BUILD}/character_cache_test"
"${TEST_BUILD}/character_cache_test"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra \
  -I"${ROOT}/include" -I"${ROOT}/platform/3ds/source" \
  "${ROOT}/tests/native_scene_test.cpp" \
  "${ROOT}/platform/3ds/source/native_engine/scene.cpp" \
  "${ROOT}/src/render/gpu_raster.cpp" -o "${TEST_BUILD}/native_scene_test"
"${TEST_BUILD}/native_scene_test"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra \
  -I"${ROOT}/tests/audio_3ds_host" -I"${ROOT}/platform/3ds/source" \
  "${ROOT}/tests/hardware_profile_test.cpp" \
  "${ROOT}/platform/3ds/source/audio_3ds.cpp" -o "${TEST_BUILD}/hardware_profile_test"
"${TEST_BUILD}/hardware_profile_test"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -pthread \
  -I"${ROOT}/tests/audio_3ds_host" -I"${ROOT}/platform/3ds/source" \
  "${ROOT}/tests/audio_suspend_test.cpp" \
  "${ROOT}/platform/3ds/source/audio_3ds.cpp" -o "${TEST_BUILD}/audio_suspend_test"
"${TEST_BUILD}/audio_suspend_test"
"${CC:-cc}" -std=c11 -O2 -Wall -Wextra -I"${ROOT}/tests/gpu_3ds_host" \
  -c "${ROOT}/platform/3ds/source/gpu_lifetime_3ds.c" -o "${TEST_BUILD}/gpu_lifetime.o"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -pthread -I"${ROOT}/tests/gpu_3ds_host" \
  "${ROOT}/tests/gpu_lifetime_test.cpp" "${TEST_BUILD}/gpu_lifetime.o" -o "${TEST_BUILD}/gpu_lifetime_test"
"${TEST_BUILD}/gpu_lifetime_test"
"${TEST_BUILD}/gpu_lifetime_test" callback-race
python3 "${ROOT}/platform/3ds/tools/test_analyze_hardware_dump.py"

if [[ -n "${STARWING_JANSSON_PREFIX:-}" ]]; then
  JANSSON_FLAGS=(-I"${STARWING_JANSSON_PREFIX}/include" "${STARWING_JANSSON_PREFIX}/lib/libjansson.a")
else
  read -r -a JANSSON_FLAGS <<< "$(pkg-config --cflags --libs jansson)"
fi
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra \
  "${ROOT}/tests/update_manifest_test.cpp" \
  "${ROOT}/platform/3ds/source/update_manifest.cpp" \
  "${JANSSON_FLAGS[@]}" -o "${TEST_BUILD}/update_manifest_test"
"${TEST_BUILD}/update_manifest_test"

"${CXX:-c++}" -std=c++20 -O1 -g -Wall -Wextra \
  '-DSTARWING_BUILD_VERSION="1.0"' -DSTARWING_CIA_VERSION=81 \
  -I"${ROOT}/tests/updater_host" \
  "${ROOT}/tests/updater_install_test.cpp" \
  "${ROOT}/platform/3ds/source/updater_3ds.cpp" \
  "${ROOT}/platform/3ds/source/update_manifest.cpp" \
  "${JANSSON_FLAGS[@]}" -lcurl -o "${TEST_BUILD}/update_install_test"
FIXTURES="$(mktemp -d "${TEST_BUILD}/update-fixtures.XXXXXX")"
trap 'rm -rf "${FIXTURES}"' EXIT
(cd "${FIXTURES}" && "${TEST_BUILD}/update_install_test")
