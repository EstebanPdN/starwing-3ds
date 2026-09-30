#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct StarwingRomInfo {
    const char *revision;
    uint32_t crc32;
} StarwingRomInfo;

/* Returns 1 for a supported, unmodified 1 MiB retail dump, 0 otherwise. */
int starwing_probe_rom(const char *path, StarwingRomInfo *result);
int starwing_has_rom_extension(const char *name);

#ifdef __cplusplus
}
#endif
