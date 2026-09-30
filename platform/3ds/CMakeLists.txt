cmake_minimum_required(VERSION 3.20)
project(starwing_3ds_diagnostic VERSION 0.1 LANGUAGES C CXX)

set(STARFOX_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" STARWING_VERSION LIMIT_COUNT 1)
set(STARWING_PATCH_DIR "${STARFOX_SOURCE_ROOT}/assets/patches" CACHE PATH
    "Private, locally prepared BPS resources; never published with the port")
set(STARWING_SYMBOL_DIR "${STARFOX_SOURCE_ROOT}/assets/symbols" CACHE PATH
    "Locally prepared runtime symbol maps")
set(STARWING_PRESENTATION_DIR "${CMAKE_CURRENT_LIST_DIR}/assets" CACHE PATH
    "Private presentation resources used by the original local build")
set(STARWING_UI_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/source" CACHE PATH
    "Private generated UI artwork headers")

add_executable(starwing-3ds-diagnostic
    source/main_3ds.c
    source/rom_probe.c
    source/asset_stage.cpp
    source/core_probe.cpp)
target_include_directories(starwing-3ds-diagnostic PRIVATE
    "${STARFOX_SOURCE_ROOT}/include")
target_compile_features(starwing-3ds-diagnostic PRIVATE c_std_11 cxx_std_20)
target_compile_options(starwing-3ds-diagnostic PRIVATE
    -O2 -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections)
set(STARFOX_ARM_BUILD "${STARFOX_SOURCE_ROOT}/../Build/3ds-core" CACHE PATH
    "Directory containing the ARM-built Starwing core libraries")
foreach(STARFOX_ARM_LIBRARY IN ITEMS
    starfox_core retro_cpu_65816 retro_cpu_core snes_spc_core)
    add_library("${STARFOX_ARM_LIBRARY}_arm" STATIC IMPORTED)
    set_target_properties("${STARFOX_ARM_LIBRARY}_arm" PROPERTIES
        IMPORTED_LOCATION "${STARFOX_ARM_BUILD}/lib${STARFOX_ARM_LIBRARY}.a")
endforeach()
target_link_libraries(starwing-3ds-diagnostic PRIVATE
    starfox_core_arm retro_cpu_65816_arm retro_cpu_core_arm
    snes_spc_core_arm ctru m)

set(STARWING_ROMFS "${CMAKE_CURRENT_BINARY_DIR}/romfs")
file(MAKE_DIRECTORY "${STARWING_ROMFS}")
configure_file("${STARWING_PRESENTATION_DIR}/top-background.rgb"
    "${STARWING_ROMFS}/top-background.rgb" COPYONLY)
foreach(STARWING_PATCH IN ITEMS
    ultrastarfox-v12.bps starfox-ex-v12.bps
    retail-japan-v10-to-usa-v12.bps
    retail-japan-v11-to-usa-v12.bps
    retail-usa-v10-to-v12.bps
    retail-usa-v11-to-v12.bps
    retail-europe-v10-to-usa-v12.bps
    retail-europe-v11-to-usa-v12.bps
    retail-germany-v10-to-usa-v12.bps)
    configure_file("${STARWING_PATCH_DIR}/${STARWING_PATCH}"
        "${STARWING_ROMFS}/${STARWING_PATCH}" COPYONLY)
endforeach()
foreach(STARWING_SYMBOLS IN ITEMS ultrastarfox.txt starfox-ex.txt)
    configure_file("${STARWING_SYMBOL_DIR}/${STARWING_SYMBOLS}"
        "${STARWING_ROMFS}/${STARWING_SYMBOLS}" COPYONLY)
endforeach()

set(STARWING_SMDH "${CMAKE_CURRENT_BINARY_DIR}/starwing-diagnostic.smdh")
ctr_generate_smdh(
    OUTPUT "${STARWING_SMDH}"
    NAME "Starwing 3DS Diagnostic"
    DESCRIPTION "SD and ROM detection test"
    AUTHOR "Esteban PDN"
    ICON "${STARWING_PRESENTATION_DIR}/icon-48.png")

ctr_create_3dsx(starwing-diagnostic-3dsx
    TARGET starwing-3ds-diagnostic
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Starwing-3DS-Diagnostic-0.1.3dsx"
    SMDH "${STARWING_SMDH}"
    ROMFS "${STARWING_ROMFS}")

add_library(starwing_citro2d STATIC
    vendor/citro2d/source/base.c
    vendor/citro2d/source/font.c
    vendor/citro2d/source/text.c
    vendor/citro2d/source/spritesheet.c
    vendor/citro2d/source/render2d_shbin.c)
target_include_directories(starwing_citro2d PUBLIC vendor/citro2d/include)
target_compile_options(starwing_citro2d PRIVATE -O3 -ffunction-sections -fdata-sections)
target_link_libraries(starwing_citro2d PUBLIC citro3d ctru m)

add_executable(starwing-3ds-game
    source/game_3ds.cpp
    source/rom_probe.c
    source/asset_stage.cpp
    source/audio_3ds.cpp
    source/display_3ds.cpp
    source/frame_3ds.cpp
    source/native_canvas.cpp
    source/input_3ds.cpp
    source/options_menu_3ds.cpp
    source/diagnostics_3ds.cpp
    source/bg2_plan.cpp)
target_include_directories(starwing-3ds-game PRIVATE
    "${STARFOX_SOURCE_ROOT}/include" "${STARWING_UI_INCLUDE_DIR}")
target_compile_definitions(starwing-3ds-game PRIVATE
    STARWING_BUILD_VERSION="${STARWING_VERSION}")
target_compile_features(starwing-3ds-game PRIVATE c_std_11 cxx_std_20)
target_compile_options(starwing-3ds-game PRIVATE
    -O3 -Wall -Wextra -ffunction-sections -fdata-sections)
target_link_libraries(starwing-3ds-game PRIVATE
    starfox_core_arm retro_cpu_65816_arm retro_cpu_core_arm
    snes_spc_core_arm starwing_citro2d citro3d ctru m)

set(STARWING_GAME_SMDH "${CMAKE_CURRENT_BINARY_DIR}/starwing-game.smdh")
ctr_generate_smdh(
    OUTPUT "${STARWING_GAME_SMDH}"
    NAME "Starwing 3DS"
    DESCRIPTION "Native Star Fox and EX"
    AUTHOR "Esteban PDN"
    ICON "${STARWING_PRESENTATION_DIR}/icon-48.png")
ctr_create_3dsx(starwing-game-3dsx
    TARGET starwing-3ds-game
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Starwing-3DS-Game.3dsx"
    SMDH "${STARWING_GAME_SMDH}"
    ROMFS "${STARWING_ROMFS}")
