# Configure-time fingerprint, not a hash of the final executable. The manifest
# retained beside that executable documents each input and its exact digest.
file(GLOB_RECURSE STARWING_FINGERPRINT_FILES CONFIGURE_DEPENDS
    "${STARFOX_SOURCE_ROOT}/include/*.h" "${STARFOX_SOURCE_ROOT}/include/*.hpp"
    "${STARFOX_SOURCE_ROOT}/src/*.c" "${STARFOX_SOURCE_ROOT}/src/*.cpp"
    "${STARFOX_SOURCE_ROOT}/src/*.h" "${STARFOX_SOURCE_ROOT}/src/*.hpp"
    "${STARFOX_SOURCE_ROOT}/tools/cmake/*.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/source/*.c" "${CMAKE_CURRENT_LIST_DIR}/source/*.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/source/*.h" "${CMAKE_CURRENT_LIST_DIR}/source/*.hpp"
    "${CMAKE_CURRENT_LIST_DIR}/vendor/*.c" "${CMAKE_CURRENT_LIST_DIR}/vendor/*.h"
    "${STARWING_UI_INCLUDE_DIR}/*.hpp")
list(APPEND STARWING_FINGERPRINT_FILES "${STARFOX_SOURCE_ROOT}/CMakeLists.txt"
    "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" "${CMAKE_CURRENT_LIST_FILE}"
    "${CMAKE_CURRENT_LIST_DIR}/version.txt"
    "${CMAKE_CURRENT_LIST_DIR}/cia-version.txt"
    "${CMAKE_CURRENT_LIST_DIR}/cia/starwing-game.rsf")
list(REMOVE_DUPLICATES STARWING_FINGERPRINT_FILES)
list(SORT STARWING_FINGERPRINT_FILES)
string(TOUPPER "${CMAKE_BUILD_TYPE}" STARWING_BUILD_TYPE_UPPER)
set(STARWING_BUILD_INPUTS "schema=1\nkind=source-config-imported-core-sha256\nversion=${STARWING_VERSION}\ncompiler=${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}\nbuild_type=${CMAKE_BUILD_TYPE}\nc_flags=${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${STARWING_BUILD_TYPE_UPPER}}\ncxx_flags=${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${STARWING_BUILD_TYPE_UPPER}}\nlink_flags=${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${STARWING_BUILD_TYPE_UPPER}}\n")
foreach(STARWING_INPUT IN LISTS STARWING_FINGERPRINT_FILES)
    file(SHA256 "${STARWING_INPUT}" STARWING_INPUT_HASH)
    file(RELATIVE_PATH STARWING_INPUT_NAME "${STARFOX_SOURCE_ROOT}" "${STARWING_INPUT}")
    string(APPEND STARWING_BUILD_INPUTS "${STARWING_INPUT_HASH}  ${STARWING_INPUT_NAME}\n")
endforeach()
# Imported core archives are the actual linked inputs; source hashes alone
# cannot detect an accidentally stale separately built core.
foreach(STARWING_LIBRARY IN ITEMS starfox_core retro_cpu_65816 retro_cpu_core snes_spc_core)
    set(STARWING_INPUT "${STARFOX_ARM_BUILD}/lib${STARWING_LIBRARY}.a")
    if(EXISTS "${STARWING_INPUT}")
        file(SHA256 "${STARWING_INPUT}" STARWING_INPUT_HASH)
        string(APPEND STARWING_BUILD_INPUTS "${STARWING_INPUT_HASH}  imported/lib${STARWING_LIBRARY}.a\n")
        list(APPEND STARWING_FINGERPRINT_FILES "${STARWING_INPUT}")
    else()
        message(FATAL_ERROR "Build the imported ARM core before configuring the 3DS frontend: ${STARWING_INPUT}")
    endif()
endforeach()
foreach(UPDATE_LIBRARY IN ITEMS curl jansson mbedtls mbedx509 mbedcrypto)
    set(UPDATE_INPUT "${STARWING_${UPDATE_LIBRARY}_LIBRARY}")
    file(SHA256 "${UPDATE_INPUT}" UPDATE_HASH)
    string(APPEND STARWING_BUILD_INPUTS "${UPDATE_HASH}  updater/lib${UPDATE_LIBRARY}.a\n")
    list(APPEND STARWING_FINGERPRINT_FILES "${UPDATE_INPUT}")
endforeach()
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/romfs/update-ca.crt" UPDATE_CA_HASH)
string(APPEND STARWING_BUILD_INPUTS "${UPDATE_CA_HASH}  platform/3ds/romfs/update-ca.crt\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${STARWING_FINGERPRINT_FILES})
string(SHA256 STARWING_BUILD_FINGERPRINT "${STARWING_BUILD_INPUTS}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/starwing-build-inputs.sha256" "${STARWING_BUILD_INPUTS}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/starwing-build-fingerprint.txt" "${STARWING_BUILD_FINGERPRINT}\n")
