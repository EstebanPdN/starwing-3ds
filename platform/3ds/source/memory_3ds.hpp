#pragma once

#include <cstdint>

namespace starfox::platform_3ds {

struct AllocationFailure {
    const char* operation{"startup"};
    const char* flow{"BOOT"};
    std::uint32_t heap_capacity{}, heap_used{}, heap_arena{}, heap_free_arena{};
    std::uint32_t linear_capacity{}, linear_free{};
    bool captured{};
};
extern AllocationFailure allocation_failure;
void capture_allocation_failure();

} // namespace starfox::platform_3ds
