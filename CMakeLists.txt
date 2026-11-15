cmake_minimum_required(VERSION 3.22)

project(starfox_enhanced VERSION 0.0.6.7 LANGUAGES CXX)

# The application target lives in platform/3ds. This project builds its
# shared engine and optional native regression/data tools.
option(STARFOX_BUILD_TESTS "Build deterministic host engine tests" ON)
option(STARFOX_BUILD_TOOLS "Build host trace and data tools" ON)

set(STARFOX_ROM_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
    CACHE FILEPATH "Assembled UltraStarFox ROM used by the native runtime")
set(STARFOX_SYMBOLS_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/symbols/ultrastarfox.txt"
    CACHE FILEPATH "Pinned UltraStarFox symbol table used by the native runtime")
set(STARFOX_EX_ROM_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-star-fox-ex/SFES/SFES.SFC"
    CACHE FILEPATH "Assembled Star Fox EX ROM used by the native runtime")
set(STARFOX_EX_SYMBOLS_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/symbols/starfox-ex.txt"
    CACHE FILEPATH "Pinned Star Fox EX symbol table used by the native runtime")
set(STARFOX_ORIGINAL_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/ultrastarfox-v12.bps"
    CACHE FILEPATH "UltraStarFox BPS delta for a clean Star Fox USA v1.2 ROM")
set(STARFOX_EX_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/starfox-ex-v12.bps"
    CACHE FILEPATH "Star Fox EX BPS delta for a clean Star Fox USA v1.2 ROM")
if(WIN32)
    set(STARFOX_RETAIL_ROM_DIR "C:/NTSC-US Super Nintendo System Roms"
        CACHE PATH "Directory searched for clean retail validation ROMs")
else()
    set(STARFOX_RETAIL_ROM_DIR "$ENV{HOME}/NTSC-US Super Nintendo System Roms"
        CACHE PATH "Directory searched for clean retail validation ROMs")
endif()
set(STARFOX_RETAIL_V12_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Star Fox (USA) (Rev 2).sfc"
    CACHE FILEPATH "Clean Star Fox USA v1.2 ROM used only for local validation")
set(STARFOX_RETAIL_JAPAN_V10_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Star Fox (Japan).sfc")
set(STARFOX_RETAIL_JAPAN_V11_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Star Fox (Japan) (Rev 1).sfc")
set(STARFOX_RETAIL_USA_V10_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Star Fox (USA).sfc")
set(STARFOX_RETAIL_USA_V11_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Star Fox (USA) (Rev 1).sfc")
set(STARFOX_RETAIL_EUROPE_V10_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Starwing (Europe).sfc")
set(STARFOX_RETAIL_EUROPE_V11_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Starwing (Europe) (Rev 1).sfc")
set(STARFOX_RETAIL_GERMANY_V10_FILE
    "${STARFOX_RETAIL_ROM_DIR}/Starwing (Germany).sfc")
set(STARFOX_RETAIL_JAPAN_V10_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/retail-japan-v10-to-usa-v12.bps")
set(STARFOX_RETAIL_JAPAN_V11_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/retail-japan-v11-to-usa-v12.bps")
set(STARFOX_RETAIL_USA_V10_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/retail-usa-v10-to-v12.bps")
set(STARFOX_RETAIL_USA_V11_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/retail-usa-v11-to-v12.bps")
set(STARFOX_RETAIL_EUROPE_V10_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/retail-europe-v10-to-usa-v12.bps")
set(STARFOX_RETAIL_EUROPE_V11_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/retail-europe-v11-to-usa-v12.bps")
set(STARFOX_RETAIL_GERMANY_V10_PATCH_FILE
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/patches/retail-germany-v10-to-usa-v12.bps")

include(FetchContent)
FetchContent_Declare(
    retro_cpu
    GIT_REPOSITORY https://github.com/achaulk/retro_cpu.git
    GIT_TAG ea9049ab25084334f7cc1907b3a98bf1c2604a03
    SOURCE_SUBDIR _starfox_no_upstream_project
)
FetchContent_MakeAvailable(retro_cpu)

FetchContent_Declare(
    snes_spc
    GIT_REPOSITORY https://github.com/blarggs-audio-libraries/snes_spc.git
    GIT_TAG ec8ee2bbe30451614c1d02a83f7af1c97d497d45
    SOURCE_SUBDIR _starfox_no_upstream_project
)
FetchContent_MakeAvailable(snes_spc)

include(tools/cmake/SpcState.cmake)
add_library(snes_spc_core STATIC
    "${STARFOX_SPC_SOURCE}/spc.cpp"
    "${STARFOX_SPC_SOURCE}/SNES_SPC.cpp"
    "${STARFOX_SPC_SOURCE}/SNES_SPC_misc.cpp"
    "${STARFOX_SPC_SOURCE}/SNES_SPC_state.cpp"
    "${STARFOX_SPC_SOURCE}/SPC_DSP.cpp"
    "${STARFOX_SPC_SOURCE}/SPC_Filter.cpp"
)
target_include_directories(snes_spc_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(snes_spc_core PUBLIC cxx_std_17)
target_include_directories(snes_spc_core SYSTEM PUBLIC
    "${STARFOX_SPC_SOURCE}"
)

# RetroCPU's top-level build includes an unused legacy host layer that does
# not compile on modern MinGW. Build only the MIT CPU/bus sources we use.
include(tools/cmake/RetroCpuFastBus.cmake)
add_library(retro_cpu_core STATIC
    "${STARFOX_RETRO_CPU_SOURCE}/cpu.cc"
)
target_include_directories(retro_cpu_core SYSTEM PUBLIC "${STARFOX_RETRO_CPU_SOURCE}")
target_compile_features(retro_cpu_core PUBLIC cxx_std_17)
add_library(retro_cpu_65816 STATIC
    "${STARFOX_RETRO_CPU_SOURCE}/cpu/65816/cpu_65c816.cc"
)
target_include_directories(retro_cpu_65816 SYSTEM PUBLIC "${STARFOX_RETRO_CPU_SOURCE}")
target_link_libraries(retro_cpu_65816 PUBLIC retro_cpu_core)
target_compile_features(retro_cpu_65816 PUBLIC cxx_std_17)
option(STARFOX_VERIFY_FAST_65816
    "Compare every instruction of the fast 65C816 loop with RetroCPU (slow)" OFF)
if(STARFOX_VERIFY_FAST_65816)
    target_compile_definitions(retro_cpu_core PUBLIC STARFOX_VERIFY_FAST_65816)
endif()
if(MINGW)
    target_compile_definitions(retro_cpu_core PUBLIC __FUNCSIG__=__PRETTY_FUNCTION__)
    target_compile_definitions(retro_cpu_65816 PUBLIC __FUNCSIG__=__PRETTY_FUNCTION__)
endif()

add_library(starfox_core
    src/audio/spc700_audio.cpp
    src/assets/bps.cpp
    src/assets/decrunch.cpp
    src/assets/rom.cpp
    src/assets/runtime_bundle.cpp
    src/assets/shape_decoder.cpp
    src/render/background_renderer.cpp
    src/render/dust_renderer.cpp
    src/render/gpu_raster.cpp
    src/render/scalefx.cpp
    src/render/framebuffer.cpp
    src/render/palette.cpp
    src/render/particle_renderer.cpp
    src/render/pixel_filter.cpp
    src/render/presentation_history.cpp
    src/render/row_workers.cpp
    src/render/scaled_text_renderer.cpp
    src/render/software_renderer.cpp
    src/render/face_material.cpp
    src/render/sprite_renderer.cpp
    src/simulation/math.cpp
    src/simulation/dust_system.cpp
    src/simulation/game_simulation.cpp
    src/simulation/game_state.cpp
    src/simulation/map_vm.cpp
    src/simulation/object_pool.cpp
    src/simulation/state_container.cpp
    src/simulation/state_files.cpp
    src/simulation/particle_system.cpp
    src/simulation/path_vm.cpp
    src/simulation/prng.cpp
    src/simulation/strategy_scheduler.cpp
    src/simulation/wdc65816.cpp
    src/simulation/wdc65816_kernels.cpp
    src/simulation/wdc65816_lean.cpp
    src/timing/fixed_step.cpp
)

target_include_directories(starfox_core PUBLIC include)
target_include_directories(starfox_core PRIVATE src/simulation)
set(STARFOX_PRIVATE_INCLUDE_DIR "" CACHE PATH
    "Private generated game-data headers, kept outside the public repository")
if(STARFOX_PRIVATE_INCLUDE_DIR)
    target_include_directories(starfox_core BEFORE PRIVATE "${STARFOX_PRIVATE_INCLUDE_DIR}")
endif()
target_compile_features(starfox_core PUBLIC cxx_std_20)
find_package(Threads REQUIRED)
target_link_libraries(starfox_core PRIVATE
    retro_cpu_65816 snes_spc_core Threads::Threads)

if(MSVC)
    target_compile_options(starfox_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(starfox_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(STARFOX_BUILD_TOOLS)
    add_executable(starfox_timing_demo src/app/timing_demo.cpp)
    target_link_libraries(starfox_timing_demo PRIVATE starfox_core)

    add_executable(starfox_shape_preview src/app/shape_preview.cpp)
    target_link_libraries(starfox_shape_preview PRIVATE starfox_core)

    add_executable(starfox_map_trace src/app/map_trace.cpp)
    target_link_libraries(starfox_map_trace PRIVATE starfox_core)

    add_executable(starfox_strategy_trace src/app/strategy_trace.cpp)
    target_link_libraries(starfox_strategy_trace PRIVATE starfox_core)

    add_executable(starfox_game_init_trace src/app/game_init_trace.cpp)
    target_link_libraries(starfox_game_init_trace PRIVATE starfox_core)

    add_executable(starfox_stage_trace src/app/stage_trace.cpp)
    target_link_libraries(starfox_stage_trace PRIVATE starfox_core)

    add_executable(starfox_stage_preview src/app/stage_preview.cpp)
    target_link_libraries(starfox_stage_preview PRIVATE starfox_core)

    add_executable(starfox_planet_probe src/app/planet_probe.cpp)
    target_link_libraries(starfox_planet_probe PRIVATE starfox_core)
endif()

if(STARFOX_BUILD_TESTS)
    if(STARFOX_PRIVATE_INCLUDE_DIR)
        include_directories("${STARFOX_PRIVATE_INCLUDE_DIR}")
    endif()
    enable_testing()
    add_executable(starfox_dsp_counter_tests tests/dsp_counter_test.cpp)
    target_link_libraries(starfox_dsp_counter_tests PRIVATE starfox_core)
    add_test(NAME starfox_dsp_counter_tests COMMAND starfox_dsp_counter_tests)
    add_executable(starfox_brr_decode_tests tests/brr_decode_test.cpp)
    target_link_libraries(starfox_brr_decode_tests PRIVATE starfox_core)
    add_test(NAME starfox_brr_decode_tests COMMAND starfox_brr_decode_tests)
    add_executable(starfox_source_sprite_material_tests tests/source_sprite_material_tests.cpp)
    target_link_libraries(starfox_source_sprite_material_tests PRIVATE starfox_core)
    if(EXISTS "${STARFOX_ROM_FILE}" AND EXISTS "${STARFOX_SYMBOLS_FILE}")
        add_test(NAME starfox_source_sprite_material_tests COMMAND starfox_source_sprite_material_tests
            "${STARFOX_ROM_FILE}" "${STARFOX_SYMBOLS_FILE}")
    endif()
    add_executable(starfox_mode2_phase_tests tests/mode2_phase_test.cpp
        platform/3ds/source/bg2_plan.cpp)
    target_include_directories(starfox_mode2_phase_tests PRIVATE platform/3ds/source)
    target_link_libraries(starfox_mode2_phase_tests PRIVATE starfox_core)
    add_test(NAME starfox_mode2_phase_tests COMMAND starfox_mode2_phase_tests)
    add_executable(starfox_bg2_plan_tests
        tests/bg2_plan_test.cpp
        platform/3ds/source/bg2_plan.cpp)
    target_link_libraries(starfox_bg2_plan_tests PRIVATE starfox_core)
    add_test(NAME starfox_bg2_plan_tests COMMAND starfox_bg2_plan_tests)
    add_executable(starfox_bg2_atlas_cache_tests
        tests/bg2_atlas_cache_test.cpp platform/3ds/source/bg2_plan.cpp)
    target_link_libraries(starfox_bg2_atlas_cache_tests PRIVATE starfox_core)
    add_test(NAME starfox_bg2_atlas_cache_tests COMMAND starfox_bg2_atlas_cache_tests)
    add_executable(starfox_bg2_map_cache_tests tests/bg2_map_cache_test.cpp)
    target_link_libraries(starfox_bg2_map_cache_tests PRIVATE starfox_core)
    add_test(NAME starfox_bg2_map_cache_tests COMMAND starfox_bg2_map_cache_tests)
    add_executable(starfox_bps_tests tests/bps_tests.cpp)
    target_link_libraries(starfox_bps_tests PRIVATE starfox_core)
    add_test(NAME starfox_bps_tests COMMAND starfox_bps_tests)
    add_executable(starfox_pixel_filter_tests tests/pixel_filter_tests.cpp)
    target_link_libraries(starfox_pixel_filter_tests PRIVATE starfox_core)
    add_test(
        NAME starfox_pixel_filter_tests COMMAND starfox_pixel_filter_tests)
    if(EXISTS "${STARFOX_RETAIL_V12_FILE}"
       AND EXISTS "${STARFOX_ORIGINAL_PATCH_FILE}"
       AND EXISTS "${STARFOX_ROM_FILE}"
       AND EXISTS "${STARFOX_EX_PATCH_FILE}"
       AND EXISTS "${STARFOX_EX_ROM_FILE}")
        add_test(
            NAME starfox_retail_v12_reconstruction
            COMMAND starfox_bps_tests
                "${STARFOX_RETAIL_V12_FILE}"
                "${STARFOX_ORIGINAL_PATCH_FILE}"
                "${STARFOX_ROM_FILE}"
                "${STARFOX_EX_PATCH_FILE}"
                "${STARFOX_EX_ROM_FILE}")
    endif()
    if(EXISTS "${STARFOX_RETAIL_V12_FILE}"
       AND EXISTS "${STARFOX_RETAIL_JAPAN_V10_FILE}"
       AND EXISTS "${STARFOX_RETAIL_JAPAN_V11_FILE}"
       AND EXISTS "${STARFOX_RETAIL_USA_V10_FILE}"
       AND EXISTS "${STARFOX_RETAIL_USA_V11_FILE}"
       AND EXISTS "${STARFOX_RETAIL_EUROPE_V10_FILE}"
       AND EXISTS "${STARFOX_RETAIL_EUROPE_V11_FILE}"
       AND EXISTS "${STARFOX_RETAIL_GERMANY_V10_FILE}"
       AND EXISTS "${STARFOX_RETAIL_JAPAN_V10_PATCH_FILE}"
       AND EXISTS "${STARFOX_RETAIL_JAPAN_V11_PATCH_FILE}"
       AND EXISTS "${STARFOX_RETAIL_USA_V10_PATCH_FILE}"
       AND EXISTS "${STARFOX_RETAIL_USA_V11_PATCH_FILE}"
       AND EXISTS "${STARFOX_RETAIL_EUROPE_V10_PATCH_FILE}"
       AND EXISTS "${STARFOX_RETAIL_EUROPE_V11_PATCH_FILE}"
       AND EXISTS "${STARFOX_RETAIL_GERMANY_V10_PATCH_FILE}")
        add_test(
            NAME starfox_retail_variant_canonicalization
            COMMAND starfox_bps_tests --verify-canonicalizations
                "${STARFOX_RETAIL_V12_FILE}"
                "${STARFOX_RETAIL_JAPAN_V10_FILE}"
                "${STARFOX_RETAIL_JAPAN_V10_PATCH_FILE}"
                "${STARFOX_RETAIL_JAPAN_V11_FILE}"
                "${STARFOX_RETAIL_JAPAN_V11_PATCH_FILE}"
                "${STARFOX_RETAIL_USA_V10_FILE}"
                "${STARFOX_RETAIL_USA_V10_PATCH_FILE}"
                "${STARFOX_RETAIL_USA_V11_FILE}"
                "${STARFOX_RETAIL_USA_V11_PATCH_FILE}"
                "${STARFOX_RETAIL_EUROPE_V10_FILE}"
                "${STARFOX_RETAIL_EUROPE_V10_PATCH_FILE}"
                "${STARFOX_RETAIL_EUROPE_V11_FILE}"
                "${STARFOX_RETAIL_EUROPE_V11_PATCH_FILE}"
                "${STARFOX_RETAIL_GERMANY_V10_FILE}"
                "${STARFOX_RETAIL_GERMANY_V10_PATCH_FILE}")
    endif()
    add_executable(starfox_core_tests tests/timing_tests.cpp)
    add_executable(starfox_frame_pacing_tests tests/frame_pacing_test.cpp)
    target_compile_features(starfox_frame_pacing_tests PRIVATE cxx_std_20)
    add_test(NAME starfox_frame_pacing_tests COMMAND starfox_frame_pacing_tests)
    add_executable(starfox_presentation_metrics_tests tests/presentation_metrics_test.cpp)
    target_compile_features(starfox_presentation_metrics_tests PRIVATE cxx_std_20)
    add_test(NAME starfox_presentation_metrics_tests COMMAND starfox_presentation_metrics_tests)
    add_executable(starfox_performance_history_tests tests/performance_history_test.cpp)
    target_compile_features(starfox_performance_history_tests PRIVATE cxx_std_20)
    add_test(NAME starfox_performance_history_tests COMMAND starfox_performance_history_tests)
    add_executable(starfox_bg2_motion_tests tests/bg2_motion_test.cpp)
    target_link_libraries(starfox_bg2_motion_tests PRIVATE starfox_core)
    add_test(NAME starfox_bg2_motion_tests COMMAND starfox_bg2_motion_tests)
    add_executable(starfox_diagnostic_sha256_tests tests/diagnostic_sha256_test.cpp)
    target_compile_features(starfox_diagnostic_sha256_tests PRIVATE cxx_std_20)
    add_test(NAME starfox_diagnostic_sha256_tests COMMAND starfox_diagnostic_sha256_tests)
    add_executable(starfox_3ds_frame_tests tests/frame_3ds_test.cpp
        platform/3ds/source/frame_3ds.cpp platform/3ds/source/native_canvas.cpp
        platform/3ds/source/native_engine/scene.cpp)
    target_include_directories(starfox_3ds_frame_tests PRIVATE tests/3ds_host)
    target_link_libraries(starfox_3ds_frame_tests PRIVATE starfox_core)
    add_executable(starfox_mode1_bg2_atlas_tests tests/mode1_bg2_atlas_test.cpp)
    target_link_libraries(starfox_mode1_bg2_atlas_tests PRIVATE starfox_core)
    add_test(NAME starfox_mode1_bg2_atlas_tests COMMAND starfox_mode1_bg2_atlas_tests)
    add_executable(starfox_mode1_layer_plan_tests tests/mode1_layer_plan_test.cpp
        platform/3ds/source/native_canvas.cpp)
    target_link_libraries(starfox_mode1_layer_plan_tests PRIVATE starfox_core)
    add_test(NAME starfox_mode1_layer_plan_tests COMMAND starfox_mode1_layer_plan_tests)
    add_executable(starfox_indexed_atlas_updates_tests tests/indexed_atlas_updates_test.cpp)
    target_link_libraries(starfox_indexed_atlas_updates_tests PRIVATE starfox_core)
    add_test(NAME starfox_indexed_atlas_updates_tests COMMAND starfox_indexed_atlas_updates_tests)
    add_executable(starfox_native_mode1_canvas_tests tests/native_mode1_canvas_test.cpp
        platform/3ds/source/native_canvas.cpp)
    target_link_libraries(starfox_native_mode1_canvas_tests PRIVATE starfox_core)
    add_test(NAME starfox_native_mode1_canvas_tests COMMAND starfox_native_mode1_canvas_tests)
    add_executable(starfox_3ds_stereo_tests tests/stereo_3ds_test.cpp
        platform/3ds/source/frame_3ds.cpp platform/3ds/source/native_canvas.cpp
        platform/3ds/source/native_engine/scene.cpp)
    target_include_directories(starfox_3ds_stereo_tests PRIVATE tests/3ds_host)
    target_link_libraries(starfox_3ds_stereo_tests PRIVATE starfox_core)
    add_test(NAME starfox_3ds_stereo_tests COMMAND starfox_3ds_stereo_tests)
    add_executable(starfox_collision_pair_tests tests/collision_pair_test.cpp)
    target_link_libraries(starfox_collision_pair_tests PRIVATE starfox_core)
    add_executable(starfox_deferred_apu_sync_tests tests/deferred_apu_sync_test.cpp)
    target_link_libraries(starfox_deferred_apu_sync_tests PRIVATE starfox_core)
    target_compile_features(starfox_deferred_apu_sync_tests PRIVATE cxx_std_20)
    add_test(NAME starfox_deferred_apu_sync_tests COMMAND starfox_deferred_apu_sync_tests)
    add_executable(starfox_audio_early_ports_tests tests/audio_early_ports_test.cpp)
    target_link_libraries(starfox_audio_early_ports_tests PRIVATE starfox_core)
    # This replay takes a locally supplied ROM and symbol map; no cartridge
    # contents or fixture paths are embedded in the public test binary.
    add_executable(starfox_native_spc_dsp_copy_tests tests/native_spc_dsp_copy_test.cpp)
    target_link_libraries(starfox_native_spc_dsp_copy_tests PRIVATE snes_spc_core)
    target_compile_features(starfox_native_spc_dsp_copy_tests PRIVATE cxx_std_20)
    add_test(NAME starfox_native_spc_dsp_copy_tests COMMAND starfox_native_spc_dsp_copy_tests)
    add_executable(starfox_spc_port_pipeline_tests tests/spc_port_pipeline_test.cpp)
    target_link_libraries(starfox_spc_port_pipeline_tests PRIVATE snes_spc_core)
    target_compile_features(starfox_spc_port_pipeline_tests PRIVATE cxx_std_20)
    add_test(NAME starfox_spc_port_pipeline_tests COMMAND starfox_spc_port_pipeline_tests)
    add_executable(starfox_audio_ports_fence_tests tests/audio_ports_fence_test.cpp
        platform/3ds/source/audio_3ds.cpp)
    target_include_directories(starfox_audio_ports_fence_tests PRIVATE
        tests/audio_3ds_host platform/3ds/source)
    target_compile_features(starfox_audio_ports_fence_tests PRIVATE cxx_std_20)
    target_compile_definitions(starfox_audio_ports_fence_tests PRIVATE STARWING_AUDIO_REAL_THREADS)
    target_link_libraries(starfox_audio_ports_fence_tests PRIVATE Threads::Threads)
    add_test(NAME starfox_audio_ports_fence_tests COMMAND starfox_audio_ports_fence_tests)
    set_tests_properties(starfox_audio_ports_fence_tests PROPERTIES TIMEOUT 20)
    add_executable(starfox_collision_accelerator_tests tests/collision_accelerator_tests.cpp)
    target_link_libraries(starfox_collision_accelerator_tests PRIVATE starfox_core retro_cpu_65816)
    add_executable(starfox_transfer_accelerator_tests tests/transfer_accelerator_tests.cpp)
    target_link_libraries(starfox_transfer_accelerator_tests PRIVATE starfox_core retro_cpu_65816)
    add_executable(starfox_strategy_batch_tests tests/strategy_batch_tests.cpp)
    target_link_libraries(starfox_strategy_batch_tests PRIVATE starfox_core)
    add_executable(starfox_wdc_entry_tests tests/wdc65816_entry_tests.cpp)
    target_include_directories(starfox_wdc_entry_tests PRIVATE include)
    target_compile_features(starfox_wdc_entry_tests PRIVATE cxx_std_20)
    target_link_libraries(starfox_wdc_entry_tests PRIVATE retro_cpu_65816)
    add_test(NAME starfox_wdc_entry_tests COMMAND starfox_wdc_entry_tests)
    add_executable(starfox_3ds_presentation_tests tests/presentation_3ds_test.cpp)
    target_link_libraries(starfox_3ds_presentation_tests PRIVATE starfox_core)
    add_test(NAME starfox_3ds_presentation_tests COMMAND starfox_3ds_presentation_tests)
    add_executable(starfox_transparent_layer_tests tests/transparent_layer_tests.cpp)
    target_link_libraries(starfox_transparent_layer_tests PRIVATE starfox_core)
    add_test(NAME starfox_transparent_layer_tests COMMAND starfox_transparent_layer_tests)
    add_executable(starfox_sprite_source_cache_tests tests/sprite_source_cache_test.cpp)
    target_include_directories(starfox_sprite_source_cache_tests PRIVATE platform/3ds/source)
    target_link_libraries(starfox_sprite_source_cache_tests PRIVATE starfox_core)
    add_test(NAME starfox_sprite_source_cache_tests COMMAND starfox_sprite_source_cache_tests)
    add_executable(starfox_framebuffer_transparent_tests tests/framebuffer_transparent_test.cpp)
    target_link_libraries(starfox_framebuffer_transparent_tests PRIVATE starfox_core)
    add_test(NAME starfox_framebuffer_transparent_tests COMMAND starfox_framebuffer_transparent_tests)
    if(EXISTS "${STARFOX_ROM_FILE}" AND EXISTS "${STARFOX_SYMBOLS_FILE}")
        add_test(NAME starfox_3ds_frame_tests COMMAND starfox_3ds_frame_tests
            "${STARFOX_ROM_FILE}" "${STARFOX_SYMBOLS_FILE}")
        add_test(NAME starfox_collision_pair_original COMMAND starfox_collision_pair_tests
            "${STARFOX_ROM_FILE}" "${STARFOX_SYMBOLS_FILE}")
        add_test(NAME starfox_collision_accelerator_original COMMAND starfox_collision_accelerator_tests
            "${STARFOX_ROM_FILE}" "${STARFOX_SYMBOLS_FILE}")
        add_test(NAME starfox_transfer_accelerator_original COMMAND starfox_transfer_accelerator_tests
            "${STARFOX_ROM_FILE}" "${STARFOX_SYMBOLS_FILE}")
        add_test(NAME starfox_strategy_batch_original COMMAND starfox_strategy_batch_tests
            "${STARFOX_ROM_FILE}" "${STARFOX_SYMBOLS_FILE}")
    endif()
    if(EXISTS "${STARFOX_EX_ROM_FILE}" AND EXISTS "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_collision_pair_ex COMMAND starfox_collision_pair_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_collision_accelerator_ex COMMAND starfox_collision_accelerator_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_transfer_accelerator_ex COMMAND starfox_transfer_accelerator_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_strategy_batch_ex COMMAND starfox_strategy_batch_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
    endif()

    add_executable(starfox_state_tests tests/state_tests.cpp)
    add_executable(starfox_title_logo_check tests/title_logo_tests.cpp)
    target_link_libraries(starfox_title_logo_check PRIVATE starfox_core)
    if(STARFOX_PRIVATE_INCLUDE_DIR)
        target_include_directories(starfox_title_logo_check PRIVATE "${STARFOX_PRIVATE_INCLUDE_DIR}")
    endif()
    add_executable(starfox_background_transition_check tools/check_background_transitions.cpp)
    target_link_libraries(starfox_background_transition_check PRIVATE starfox_core)
    target_link_libraries(starfox_state_tests PRIVATE starfox_core)
    add_test(NAME starfox_state_tests COMMAND starfox_state_tests)
    add_executable(starfox_shadow_geometry_tests tests/shadow_geometry_tests.cpp)
    target_include_directories(starfox_shadow_geometry_tests PRIVATE include)
    target_compile_features(starfox_shadow_geometry_tests PRIVATE cxx_std_20)
    target_link_libraries(starfox_shadow_geometry_tests PRIVATE starfox_core)
    add_test(NAME starfox_shadow_geometry_tests COMMAND starfox_shadow_geometry_tests)
    add_executable(starfox_raster_commands_tests tools/benchmark_gpu_raster.cpp)
    add_executable(starfox_face_material_tests tests/face_material_tests.cpp)
    add_executable(starfox_word_renderer_tests tests/word_renderer_tests.cpp)
    target_link_libraries(starfox_word_renderer_tests PRIVATE starfox_core)
    add_test(NAME starfox_word_renderer_tests COMMAND starfox_word_renderer_tests)
    add_executable(starfox_source_line_runs_tests tests/source_line_runs_test.cpp)
    target_link_libraries(starfox_source_line_runs_tests PRIVATE starfox_core)
    add_test(NAME starfox_source_line_runs_tests COMMAND starfox_source_line_runs_tests)
    add_executable(starfox_source_depth_tables_tests tests/source_depth_tables_test.cpp)
    target_link_libraries(starfox_source_depth_tables_tests PRIVATE starfox_core)
    add_test(NAME starfox_source_depth_tables_tests COMMAND starfox_source_depth_tables_tests)
    add_executable(starfox_bg3_atlas_cache_tests tests/bg3_atlas_cache_test.cpp)
    target_link_libraries(starfox_bg3_atlas_cache_tests PRIVATE starfox_core)
    add_test(NAME starfox_bg3_atlas_cache_tests COMMAND starfox_bg3_atlas_cache_tests)
    target_link_libraries(starfox_face_material_tests PRIVATE starfox_core)
    add_test(NAME starfox_face_material_tests COMMAND starfox_face_material_tests)
    target_link_libraries(starfox_raster_commands_tests PRIVATE starfox_core)
    add_test(NAME starfox_raster_commands_tests COMMAND starfox_raster_commands_tests)
    target_link_libraries(starfox_core_tests PRIVATE starfox_core)
    add_test(NAME starfox_core_tests COMMAND starfox_core_tests)

    add_executable(starfox_shape_decoder_tests tests/shape_decoder_tests.cpp)
    target_link_libraries(starfox_shape_decoder_tests PRIVATE starfox_core)
    add_test(NAME starfox_shape_decoder_tests COMMAND starfox_shape_decoder_tests)

    add_executable(starfox_simulation_tests tests/simulation_tests.cpp)
    target_link_libraries(starfox_simulation_tests PRIVATE starfox_core)
    # The cartridge-wide integration fixture intentionally keeps several full
    # GameSimulation snapshots alive in nested parity sections. PE/COFF's
    # small default stack can overflow after earlier sections begin reaching
    # their now-correct transition endpoints, masking the actual assertions.
    if(MINGW)
        target_link_options(
            starfox_simulation_tests PRIVATE "-Wl,--stack,16777216")
    elseif(MSVC)
        target_link_options(starfox_simulation_tests PRIVATE "/STACK:16777216")
    endif()
    add_test(NAME starfox_simulation_tests COMMAND starfox_simulation_tests)

    add_executable(starfox_direct_audio_tests tests/direct_audio_tests.cpp)
    target_link_libraries(starfox_direct_audio_tests PRIVATE starfox_core)

    add_executable(starfox_transition_parity_tests
        tests/transition_parity_tests.cpp)
    target_link_libraries(starfox_transition_parity_tests PRIVATE starfox_core)

    add_executable(starfox_ending_tests tests/ending_tests.cpp)
    target_link_libraries(starfox_ending_tests PRIVATE starfox_core)
    add_test(NAME starfox_ending_painter_order COMMAND starfox_ending_tests --painter-order-only)
    add_executable(starfox_credits_restart_tests tests/credits_restart_tests.cpp)
    target_link_libraries(starfox_credits_restart_tests PRIVATE starfox_core)
    add_executable(starfox_intro_pacing_tests tests/intro_pacing_tests.cpp)
    add_executable(starfox_geometry_pacing_tests tests/geometry_pacing_tests.cpp)
    target_link_libraries(starfox_geometry_pacing_tests PRIVATE starfox_core)
    target_link_libraries(starfox_intro_pacing_tests PRIVATE starfox_core)
    if(MINGW)
        target_link_options(starfox_intro_pacing_tests PRIVATE "-Wl,--stack,16777216")
    elseif(MSVC)
        target_link_options(starfox_intro_pacing_tests PRIVATE "/STACK:16777216")
    endif()

    add_executable(starfox_hitlist_tests tests/hitlist_tests.cpp)
    add_executable(starfox_level_clear_tests tests/level_clear_tests.cpp)
    target_link_libraries(starfox_level_clear_tests PRIVATE starfox_core)
    target_link_libraries(starfox_hitlist_tests PRIVATE starfox_core)

    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
       AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT")
        add_test(
            NAME starfox_upstream_simulation_data
            COMMAND starfox_simulation_tests
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
        )
        add_test(NAME starfox_upstream_state_data COMMAND starfox_state_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT")
        add_test(
            NAME starfox_upstream_direct_audio
            COMMAND starfox_direct_audio_tests
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
        )
        add_test(
            NAME starfox_upstream_transition_parity
            COMMAND starfox_transition_parity_tests
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
        )
        add_test(NAME starfox_upstream_ending COMMAND starfox_ending_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT")
        add_test(NAME starfox_upstream_intro_pacing COMMAND starfox_intro_pacing_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT")
        add_test(NAME starfox_upstream_geometry_pacing COMMAND starfox_geometry_pacing_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT")
        add_test(NAME starfox_upstream_hitlist COMMAND starfox_hitlist_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT")
        add_test(NAME starfox_upstream_level_clear COMMAND starfox_level_clear_tests
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
            "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT")
        add_test(
            NAME starfox_upstream_map_trace
            COMMAND starfox_map_trace
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
                    MAP1_1A
        )
        add_test(
            NAME starfox_upstream_strategy_trace
            COMMAND starfox_strategy_trace
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
                    MAP1_1A
                    1
        )
        add_test(
            NAME starfox_upstream_game_init
            COMMAND starfox_game_init_trace
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
                    20
        )
        add_test(
            NAME starfox_upstream_stage_trace
            COMMAND starfox_stage_trace
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
                    LEVEL1_1
                    100
        )
        add_test(
            NAME starfox_upstream_planet_probe
            COMMAND starfox_planet_probe
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
        )
        add_test(
            NAME starfox_upstream_route_lifecycle
            COMMAND starfox_stage_trace
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SF.SFC"
                    "${CMAKE_CURRENT_SOURCE_DIR}/upstream-ultrastarfox/SYMBOLS.TXT"
                    LEVEL1_3
                    500
        )
    endif()

    if(EXISTS "${STARFOX_EX_ROM_FILE}"
       AND EXISTS "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(
            NAME starfox_ex_direct_audio
            COMMAND starfox_direct_audio_tests
                    "${STARFOX_EX_ROM_FILE}"
                    "${STARFOX_EX_SYMBOLS_FILE}"
        )
        add_test(
            NAME starfox_ex_transition_parity
            COMMAND starfox_transition_parity_tests
                    "${STARFOX_EX_ROM_FILE}"
                    "${STARFOX_EX_SYMBOLS_FILE}"
        )
        add_test(NAME starfox_ex_ending COMMAND starfox_ending_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_ex_intro_pacing COMMAND starfox_intro_pacing_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_ex_geometry_pacing COMMAND starfox_geometry_pacing_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_ex_hitlist COMMAND starfox_hitlist_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(NAME starfox_ex_level_clear COMMAND starfox_level_clear_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        add_test(
            NAME starfox_ex_simulation_data
            COMMAND starfox_simulation_tests
                    "${STARFOX_EX_ROM_FILE}"
                    "${STARFOX_EX_SYMBOLS_FILE}"
        )
        add_test(NAME starfox_ex_state_data COMMAND starfox_state_tests
            "${STARFOX_EX_ROM_FILE}" "${STARFOX_EX_SYMBOLS_FILE}")
        set_tests_properties(starfox_ex_simulation_data PROPERTIES
            ENVIRONMENT "STARFOX_EX_ROUTE_AUDIT_TICKS=2000"
            # The audit boots every shipped EX stage and advances each for
            # 2,000 source ticks. Renderer/route regressions have expanded
            # its coverage enough that Windows debug-host load can exceed
            # four minutes without a hang; retain the full audit and give it
            # a deterministic margin instead of shortening stage coverage.
            TIMEOUT 420)
    endif()

    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(
            NAME starfox_dialogue_catalog_tests
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                    -p "test_dialogue_catalog.py"
        )
        set_tests_properties(starfox_dialogue_catalog_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
        )
        add_test(
            NAME starfox_shape_audit_tests
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover
                    -s "${CMAKE_CURRENT_SOURCE_DIR}/tests"
                    -p "test_shape_audit.py"
        )
        set_tests_properties(starfox_shape_audit_tests PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}"
        )
    endif()
endif()
