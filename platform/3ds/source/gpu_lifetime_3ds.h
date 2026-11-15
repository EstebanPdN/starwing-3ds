#pragma once
#include <stdbool.h>
#include <3ds/types.h>
// GPU loss quarantine and the CPU recovery scanout of the top screen, used by
// display_3ds.cpp and game_3ds.cpp. tests/gpu_lifetime_test.cpp builds the C file
// on the host against stubs in tests/gpu_3ds_host.
#ifdef __cplusplus
extern "C" {
#endif
// A lost GPU is quarantined for the rest of the process. Never fake a fence.
bool starwing_gpu_abandoned(void);
// Marks the GPU lost and stops the render queue. Idempotent and thread-safe; it
// returns only after a completion callback already running has finished.
void starwing_abandon_gpu(void);
// Reserve recovery scanout before GPU work starts; caller still flushes writes.
// Allocates two 400x240 RGB565 buffers in linear memory once; false on failure.
bool starwing_prepare_cpu_top(void);
// Top-screen buffer for CPU writes: the gfx back buffer while the GPU is alive,
// else the reserve back buffer (NULL if none was reserved). width and height may
// be NULL; they receive the physical, rotated size (240x400 for the reserve).
u8* starwing_cpu_top_framebuffer(u16* width, u16* height);
// Shows the buffer returned above: a gfx swap while the GPU is alive, else a
// direct GSP present of the reserve buffer, which then flips to the other one.
void starwing_swap_cpu_top(void);
#ifdef __cplusplus
}
#endif
