#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

// CPU-to-device handoff only. GPU/DSP-to-CPU reads require invalidation.
// These callers submit from the main thread; capability state is TU-local.
static inline Result starwing_clean_device_cache(const void* address, u32 size) {
    static bool service_only;
    if (size == 0U) return 0;
    if (!service_only) {
        const Result direct = svcStoreProcessDataCache(CUR_PROCESS_HANDLE,
            (u32)(uintptr_t)address, size);
        if (R_SUCCEEDED(direct)) return direct;
        // A loader may deny SVC 83. Preserve libctru's established service
        // path and its error result instead of making direct SVC a requirement.
        service_only = true;
    }
    return GSPGPU_FlushDataCache(address, size);
}
