#pragma once
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

/* Private ABI for the pinned SDL backend, not an SDL public API. Borrowed
 * pointers expire with the SDL device. Copy requires a native resource opened
 * on that device in COMMON state when the copy executes. Queue wait_fence
 * before submitting the copy if the producer has not completed. The
 * caller keeps it alive until the submitted SDL command buffer completes.
 * Call outside render/compute/copy passes. No CPU mapping or readback occurs. */
#define STARFOX_SDL_D3D12_DEVICE "starfox.gpu.d3d12.device.v2"
#define STARFOX_SDL_D3D12_BRIDGE "starfox.gpu.d3d12.bridge.v2"
typedef struct StarfoxSdlD3D12BridgeV2 {
    uint32_t version;
    bool (*copy_buffer)(void *command_buffer, void *native_resource,
                        void *destination_buffer, uint32_t bytes);
    // Queue a shared producer fence dependency before submitting the copy.
    bool (*wait_fence)(void *device, void *native_fence, uint64_t value);
} StarfoxSdlD3D12BridgeV2;

/* Separate optional ABI: old pinned builds retain the mask bridge. Source is
 * SDL storage, destination is an imported COMMON buffer on the SDL device.
 * Caller owns destination lifetime and finishes the copy before DXR reads. */
#define STARFOX_SDL_D3D12_GEOMETRY_BRIDGE "starfox.gpu.d3d12.geometry.v1"
typedef struct StarfoxSdlD3D12GeometryBridgeV1 {
    uint32_t version;
    bool (*copy_to_external)(void *command, void *source, void *destination, uint32_t bytes);
    bool (*signal_fence)(void *device, void *native_fence, uint64_t value);
} StarfoxSdlD3D12GeometryBridgeV1;

/* Whole, single-mip 2D textures only; exact format/extent matching. Native
 * resources belong to this device and enter/leave COMMON. Call between passes.
 * Caller retains native resources until submission completes; use the existing
 * fence bridges when the native evaluator runs on a different queue. */
#define STARFOX_SDL_D3D12_TEXTURE_BRIDGE "starfox.gpu.d3d12.texture.v1"
typedef struct StarfoxSdlD3D12TextureBridgeV1 {
    uint32_t version;
    bool (*copy_to_external)(void *command, void *sdl_texture, void *native_texture);
    bool (*copy_from_external)(void *command, void *native_texture, void *sdl_texture);
} StarfoxSdlD3D12TextureBridgeV1;

/* A native compute callback between SDL passes. Read textures enter NON_PIXEL
 * SHADER_RESOURCE, one output enters UAV; callback must preserve those states,
 * not submit/close the list, and never throw across C. SDL restores defaults
 * and descriptor heaps afterward. Bind a new SDL pipeline before further draws.
 * Resource/command pointers are borrowed only for the callback duration. */
#define STARFOX_SDL_D3D12_COMPUTE_BRIDGE "starfox.gpu.d3d12.compute.v1"
typedef bool (*StarfoxD3D12ComputeCallback)(void *user, void *command_list, void *const *native_textures, uint32_t count);
typedef struct StarfoxSdlD3D12ComputeBridgeV1 {
    uint32_t version;
    bool (*dispatch)(void *command, void *const *textures, uint32_t count,
        uint32_t output_index, StarfoxD3D12ComputeCallback callback, void *user);
} StarfoxSdlD3D12ComputeBridgeV1;

// Optional process-local hooks installed before renderer creation. Callback
// replaces one owned IDXGISwapChain3 reference, never throws, and restores it
// before destruction. Clear hooks only after restoring all owned swapchains.
#define STARFOX_SDL_D3D12_PRESENT_HOOKS "starfox.gpu.d3d12.present-hooks.v1"
typedef struct StarfoxSdlD3D12PresentHooksV1 {
    uint32_t version;
    void *user;
    bool (*swapchain)(void *user, void *native_device, void **swapchain, bool restore);
} StarfoxSdlD3D12PresentHooksV1;
#define STARFOX_SDL_D3D12_PRESENT_BRIDGE "starfox.gpu.d3d12.present.v1"
typedef struct StarfoxSdlD3D12PresentBridgeV1 {
    uint32_t version;
    bool (*restore)(void *device);
} StarfoxSdlD3D12PresentBridgeV1;
