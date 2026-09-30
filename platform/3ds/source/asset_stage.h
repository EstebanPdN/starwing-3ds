#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 = existing valid bundle, 2 = newly built bundle, 0 = failure. */
int starwing_prepare_assets(const char *rom_path, const char *output_path,
    char *message, size_t message_capacity);

#ifdef __cplusplus
}
#endif
