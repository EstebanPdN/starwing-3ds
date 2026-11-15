#pragma once

#include <stddef.h>

/* C entry point of the 3DS asset stager, called by game_3ds.cpp at startup: verifies or builds
 * the SD-card runtime bundle (original and EX ROMs plus their symbol tables) from the user's
 * retail ROM. The C++ loader for that bundle is in asset_stage.hpp. */

#ifdef __cplusplus
extern "C" {
#endif

/* Verifies the bundle at output_path against the patches and symbol tables packaged in romfs.
 * If it is missing or invalid and rom_path names a supported retail ROM (any revision known to
 * rom_probe.c; a 512-byte copier header is dropped), rebuilds it by writing output_path + ".tmp"
 * and renaming it. rom_path may be NULL or empty to verify only. Copies a status or error text
 * into message when message is not NULL.
 * 1 = existing valid bundle, 2 = newly built bundle, 0 = failure. */
int starwing_prepare_assets(const char *rom_path, const char *output_path,
    char *message, size_t message_capacity);

#ifdef __cplusplus
}
#endif
