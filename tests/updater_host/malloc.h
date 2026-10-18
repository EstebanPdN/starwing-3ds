#pragma once
#include <cstdlib>
inline void* memalign(std::size_t alignment, std::size_t bytes) {
    void* result = nullptr;
    return posix_memalign(&result, alignment, bytes) == 0 ? result : nullptr;
}
