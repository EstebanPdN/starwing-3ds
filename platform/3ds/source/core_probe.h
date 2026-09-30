#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Runs an actual ARM32 simulation of the first stage. It does not render,
// play audio, or prove a sustained display frame rate.
int starwing_core_probe(const char *bundle_path, char *message, size_t capacity);

#ifdef __cplusplus
}
#endif
