#pragma once
#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

// starwing_clean_device_cache() writes CPU data-cache lines back before a device reads the
// memory. Used by display_3ds.cpp and native_engine/pica.cpp; tests/3ds_host/cache_clean_test.py
// exercises it with SVC 83 allowed and denied.

// CPU-to-device handoff only. GPU/DSP-to-CPU reads require invalidation.
// These callers submit from the main thread; capability state is TU-local.
// Returns 0 for an empty range, else the Result of svcStoreProcessDataCache
// or, from its first failure on, of GSPGPU_FlushDataCache.
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
