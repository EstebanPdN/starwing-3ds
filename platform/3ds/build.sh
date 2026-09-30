#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO
BUILD="${ROOT}/../Build/3ds-diagnostic"
CORE_BUILD="${ROOT}/../Build/3ds-core"
HOST_DEPS="${ROOT}/../Build/host-baseline-make/_deps"
CORE_ARGS=(
  -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake"
  -DCMAKE_BUILD_TYPE=Release
  -DSTARFOX_BUILD_RUNTIME=OFF
  -DSTARFOX_BUILD_TESTS=OFF
  -DSTARFOX_BUILD_TOOLS=OFF
  -DSTARFOX_ENABLE_XBRZ=OFF
  -DSTARFOX_PACKAGE_MSU1_MUSIC=OFF
)
for DEP in retro_cpu snes_spc dr_libs; do
  if [[ -d "${HOST_DEPS}/${DEP}-src" ]]; then
    DEP_UPPER="$(printf '%s' "${DEP}" | tr '[:lower:]' '[:upper:]')"
    CORE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${DEP_UPPER}=${HOST_DEPS}/${DEP}-src")
  fi
done
cmake -S "${ROOT}" -B "${CORE_BUILD}" "${CORE_ARGS[@]}"
cmake --build "${CORE_BUILD}" --target starfox_core --parallel 4

cmake -S "${ROOT}/platform/3ds" -B "${BUILD}" \
  -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD}" --parallel 4
printf 'Diagnostic 3DSX: %s\n' "${BUILD}/Starwing-3DS-Diagnostic-0.1.3dsx"

MAKEROM="${MAKEROM:-$(command -v makerom || true)}"
BANNERTOOL="${BANNERTOOL:-$(command -v bannertool || true)}"
if [[ -x "${MAKEROM}" && -x "${BANNERTOOL}" ]]; then
  "${BANNERTOOL}" makesmdh \
    -s "Starwing 3DS Diagnostic" -l "ROM and SD test only" \
    -p "Esteban PDN" -i "${ROOT}/platform/3ds/assets/icon-48.png" \
    -f visible,nosavebackups -o "${BUILD}/starwing-diagnostic.icn"
  "${BANNERTOOL}" makebanner \
    -i "${ROOT}/platform/3ds/assets/banner-256x128.png" \
    -a "${ROOT}/platform/3ds/assets/banner-silent.wav" \
    -o "${BUILD}/starwing-diagnostic.bnr"
  (
    cd "${ROOT}"
    "${MAKEROM}" -f cia \
      -o "${BUILD}/Starwing-3DS-Diagnostic-0.1.cia" \
      -DAPP_ROMFS="${BUILD}/romfs" \
      -rsf "${ROOT}/platform/3ds/cia/starwing-diagnostic.rsf" \
      -target t -exefslogo \
      -elf "${BUILD}/starwing-3ds-diagnostic.elf" \
      -icon "${BUILD}/starwing-diagnostic.icn" \
      -banner "${BUILD}/starwing-diagnostic.bnr"
  )
  printf 'Diagnostic CIA: %s\n' "${BUILD}/Starwing-3DS-Diagnostic-0.1.cia"
fi
