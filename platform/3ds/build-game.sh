#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPRO
BUILD="${STARWING_GAME_BUILD_DIR:-${ROOT}/build-3ds/game}"
CORE_BUILD="${STARWING_CORE_BUILD_DIR:-${ROOT}/build-3ds/core}"
HOST_DEPS="${STARWING_HOST_DEPS:-${ROOT}/build-host/_deps}"
RELEASE_FLAGS="-O3 -flto -DNDEBUG"
GAME_VERSION="$(tr -d '\r\n' < "${ROOT}/platform/3ds/version.txt")"
CIA_VERSION="$(tr -d '\r\n' < "${ROOT}/platform/3ds/cia-version.txt")"
CIA_NAME="Starwing-3DS-Gameplay-${GAME_VERSION}.cia"
if [[ "${STARWING_DIAGNOSTIC:-0}" == 1 ]]; then
  # Lightweight diagnostics are always enabled; deep SD profiling stays off.
  CIA_NAME="Starwing-3DS-Hardware-Diagnostic-${GAME_VERSION}.cia"
fi
CORE_ARGS=(
  -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake"
  -DCMAKE_BUILD_TYPE=Release
  -DSTARFOX_BUILD_RUNTIME=OFF
  -DSTARFOX_BUILD_TESTS=OFF
  -DSTARFOX_BUILD_TOOLS=OFF
  -DSTARFOX_ENABLE_XBRZ=OFF
  -DSTARFOX_PACKAGE_MSU1_MUSIC=OFF
  "-DSTARFOX_PRIVATE_INCLUDE_DIR=${STARFOX_PRIVATE_INCLUDE_DIR:-}"
  "-DCMAKE_CXX_FLAGS_RELEASE=${RELEASE_FLAGS}"
)
for DEP in retro_cpu snes_spc dr_libs; do
  if [[ -d "${HOST_DEPS}/${DEP}-src" ]]; then
    DEP_UPPER="$(printf '%s' "${DEP}" | tr '[:lower:]' '[:upper:]')"
    CORE_ARGS+=("-DFETCHCONTENT_SOURCE_DIR_${DEP_UPPER}=${HOST_DEPS}/${DEP}-src")
  fi
done
# Drop any cached diagnostic flags before making the installable release.
# The devkitARM toolchain reinitializes its required architecture flags.
cmake -U CMAKE_CXX_FLAGS -S "${ROOT}" -B "${CORE_BUILD}" "${CORE_ARGS[@]}"
cmake --build "${CORE_BUILD}" --target starfox_core --parallel 4
cmake -U CMAKE_CXX_FLAGS -S "${ROOT}/platform/3ds" -B "${BUILD}" \
  -DCMAKE_TOOLCHAIN_FILE="${DEVKITPRO}/cmake/3DS.cmake" \
  -DSTARFOX_ARM_BUILD="${CORE_BUILD}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS_RELEASE="${RELEASE_FLAGS}" \
  -DCMAKE_C_FLAGS_RELEASE="-O3 -flto -DNDEBUG" \
  "-DSTARWING_PATCH_DIR=${STARWING_PATCH_DIR:-${ROOT}/assets/patches}" \
  "-DSTARWING_SYMBOL_DIR=${STARWING_SYMBOL_DIR:-${ROOT}/assets/symbols}" \
  "-DSTARWING_PRESENTATION_DIR=${STARWING_PRESENTATION_DIR:-${ROOT}/platform/3ds/assets}" \
  "-DSTARWING_UI_INCLUDE_DIR=${STARWING_UI_INCLUDE_DIR:-${ROOT}/platform/3ds/source}"
cmake --build "${BUILD}" --target starwing-game-3dsx --parallel 4
printf 'Gameplay 3DSX: %s\n' "${BUILD}/Starwing-3DS-Game.3dsx"
if [[ "${STARWING_BUILD_CIA:-1}" == 0 ]]; then
  exit 0
fi

MAKEROM="${MAKEROM:-$(command -v makerom || true)}"
BANNERTOOL="${BANNERTOOL:-$(command -v bannertool || true)}"
FFMPEG="${FFMPEG:-$(command -v ffmpeg || true)}"
STARWING_BANNER_CGFX="${STARWING_BANNER_CGFX:-${ROOT}/../assets/banners/STARFOX_3D/sculpted-v2/output/StarFox-sculpted.cgfx}"
STARWING_BANNER_AUDIO="${STARWING_BANNER_AUDIO:-${ROOT}/../splash.mp3}"
STARWING_ICON="${STARWING_ICON:-${STARWING_PRESENTATION_DIR:-${ROOT}/platform/3ds/assets}/icon-48.png}"
if [[ ! -x "${MAKEROM}" || ! -x "${BANNERTOOL}" || ! -x "${FFMPEG}" ]]; then
  printf 'makerom, bannertool, and ffmpeg are required for the CIA.\n' >&2
  exit 1
fi
if [[ ! -f "${STARWING_BANNER_CGFX}" || ! -f "${STARWING_BANNER_AUDIO}" ]]; then
  printf 'Set STARWING_BANNER_CGFX and STARWING_BANNER_AUDIO to the private CIA resources.\n' >&2
  exit 1
fi
"${BANNERTOOL}" makesmdh \
  -s "Starwing 3DS" -l "Star Fox and EX" \
  -p "Esteban PDN" -i "${STARWING_ICON}" \
  -f visible,allow3d,extendedbanner,nosavebackups \
  -o "${BUILD}/starwing-game.icn"
"${FFMPEG}" -hide_banner -loglevel error -y \
  -i "${STARWING_BANNER_AUDIO}" \
  -af 'atempo=1.01,aresample=32000,atrim=end_sample=96000' \
  -ac 2 -ar 32000 -c:a pcm_s16le \
  "${BUILD}/banner-splash-stereo.wav"
"${BANNERTOOL}" makebanner \
  -ci "${STARWING_BANNER_CGFX}" \
  -a "${BUILD}/banner-splash-stereo.wav" \
  -o "${BUILD}/starwing-game.bnr"
python3 "${ROOT}/tools/validate-banner.py" \
  "${BUILD}/starwing-game.icn" "${BUILD}/starwing-game.bnr" \
  "${STARWING_BANNER_CGFX}" "${BUILD}/banner-splash-stereo.wav"
(
  cd "${ROOT}"
  "${MAKEROM}" -f cia -ver "${CIA_VERSION}" \
    -o "${BUILD}/${CIA_NAME}" \
    -DAPP_ROMFS="${BUILD}/romfs" \
    -rsf "${ROOT}/platform/3ds/cia/starwing-game.rsf" \
    -target t -exefslogo \
    -elf "${BUILD}/starwing-3ds-game.elf" \
    -icon "${BUILD}/starwing-game.icn" \
    -banner "${BUILD}/starwing-game.bnr"
)
python3 - "${BUILD}/${CIA_NAME}" "${BUILD}/starwing-game.icn" "${BUILD}/starwing-game.bnr" "${CIA_VERSION}" <<'PY'
import pathlib
import struct
import sys

cia, icon, banner = (pathlib.Path(arg).read_bytes() for arg in sys.argv[1:4])
if icon not in cia or banner not in cia:
    raise SystemExit('CIA does not contain the exact icon and banner resources')
align = lambda size: (size + 63) & ~63
header_size, _, _, cert_size, ticket_size, tmd_size = struct.unpack_from('<IHHIII', cia)
ticket = align(header_size) + align(cert_size)
tmd = ticket + align(ticket_size)
signature_bodies = {0x10000: 0x240, 0x10001: 0x140, 0x10002: 0x80,
                    0x10003: 0x240, 0x10004: 0x140, 0x10005: 0x80}
ticket_body = ticket + signature_bodies[struct.unpack_from('>I', cia, ticket)[0]]
tmd_body = tmd + signature_bodies[struct.unpack_from('>I', cia, tmd)[0]]
expected = int(sys.argv[4])
ticket_version = struct.unpack_from('>H', cia, ticket_body + 0xa6)[0]
tmd_version = struct.unpack_from('>H', cia, tmd_body + 0x9c)[0]
if ticket_version != expected or tmd_version != expected:
    raise SystemExit(f'makerom wrote ticket/TMD versions {ticket_version}/{tmd_version}; expected {expected}. '
                     'Use a makerom build with distinct TITLE_VER_ array indices.')
if cia[ticket_body + 0x9c:ticket_body + 0xa4] != cia[tmd_body + 0x4c:tmd_body + 0x54]:
    raise SystemExit('CIA ticket and TMD title IDs disagree')
print(f'CIA ticket/TMD version validated: {expected}')
PY
printf 'Gameplay CIA candidate: %s\n' "${BUILD}/${CIA_NAME}"
