#include "rom_probe.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct KnownRom {
    uint32_t crc32;
    const char *revision;
} KnownRom;

/* These checksums are the retail validation table in src/app/starfox_pc.cpp. */
static const KnownRom known_roms[] = {
    {0x8fc4e6d0u, "Star Fox USA 1.2"},
    {0x41a60b3fu, "Star Fox Japan 1.0"},
    {0xad668a41u, "Star Fox Japan 1.1"},
    {0x0bae0941u, "Star Fox USA 1.0"},
    {0xb18676b2u, "Star Fox USA 1.1"},
    {0x865f1a71u, "Starwing Europe 1.0"},
    {0xba64da2bu, "Starwing Europe 1.1"},
    {0xb48ca238u, "Starwing Germany 1.0"},
};

int starwing_has_rom_extension(const char *name) {
    const size_t length = strlen(name);
    if (length < 5u) return 0;
    const char *extension = name + length - 4u;
    return extension[0] == '.'
        && tolower((unsigned char)extension[1]) == 's'
        && tolower((unsigned char)extension[2]) == 'f'
        && tolower((unsigned char)extension[3]) == 'c'
        ? 1
        : extension[0] == '.'
            && tolower((unsigned char)extension[1]) == 's'
            && tolower((unsigned char)extension[2]) == 'm'
            && tolower((unsigned char)extension[3]) == 'c';
}

int starwing_probe_rom(const char *path, StarwingRomInfo *result) {
    if (result != NULL) {
        result->revision = NULL;
        result->crc32 = 0u;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) return 0;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    const long size = ftell(file);
    if (size != 1048576L && size != 1049088L) {
        fclose(file);
        return 0;
    }
    if (fseek(file, size == 1049088L ? 512L : 0L, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }

    uint32_t checksum = 0xffffffffu;
    unsigned char buffer[8192];
    for (size_t remaining = 1048576u; remaining != 0u;) {
        const size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (fread(buffer, 1, wanted, file) != wanted) {
            fclose(file);
            return 0;
        }
        for (size_t i = 0; i < wanted; ++i) {
            checksum ^= buffer[i];
            for (int bit = 0; bit < 8; ++bit)
                checksum = (checksum >> 1) ^ (0xedb88320u & (0u - (checksum & 1u)));
        }
        remaining -= wanted;
    }
    fclose(file);
    checksum ^= 0xffffffffu;
    for (size_t i = 0; i < sizeof(known_roms) / sizeof(known_roms[0]); ++i) {
        if (checksum == known_roms[i].crc32) {
            if (result != NULL) {
                result->revision = known_roms[i].revision;
                result->crc32 = checksum;
            }
            return 1;
        }
    }
    return 0;
}
