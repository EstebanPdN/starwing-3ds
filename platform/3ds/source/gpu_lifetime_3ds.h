#pragma once
#include <stdbool.h>
#include <3ds/types.h>
#ifdef __cplusplus
extern "C" {
#endif
// A lost GPU is quarantined for the rest of the process. Never fake a fence.
bool starwing_gpu_abandoned(void);
void starwing_abandon_gpu(void);
// Reserve recovery scanout before GPU work starts; caller still flushes writes.
bool starwing_prepare_cpu_top(void);
u8* starwing_cpu_top_framebuffer(u16* width, u16* height);
void starwing_swap_cpu_top(void);
#ifdef __cplusplus
}
#endif
