#pragma once

#include <stddef.h>
#include <stdint.h>

/* Identifies the cartridge dump the player supplied by the CRC-32 of its 1 MiB
   image. Used by game_3ds.cpp, asset_stage.cpp and the Options ROM list. */

#ifdef __cplusplus
extern "C" {
#endif

/* revision names the retail release, e.g. "Star Fox USA 1.2" (static string);
   crc32 covers the 1 MiB image without a copier header. */
typedef struct StarwingRomInfo {
    const char *revision;
    uint32_t crc32;
} StarwingRomInfo;

/* Returns 1 for a supported, unmodified 1 MiB retail dump, 0 otherwise.
   A 512-byte copier header is skipped. `result` may be NULL; it is cleared
   first and filled only on success. */
int starwing_probe_rom(const char *path, StarwingRomInfo *result);
/* 1 when `name` has a stem and ends in .sfc or .smc, in any case. */
int starwing_has_rom_extension(const char *name);

#ifdef __cplusplus
}
#endif
