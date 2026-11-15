#pragma once

#include <cstdint>

// Out-of-memory report for the 3DS frontend. game_3ds.cpp installs
// capture_allocation_failure as the new-handler and writes allocation_failure
// to last-error.txt when the resulting exception reaches main.

namespace starfox::platform_3ds {

// operation and flow are static labels of what was running, kept current by
// game_3ds.cpp. The byte counts are valid only when `captured` is set.
struct AllocationFailure {
    const char* operation{"startup"};
    const char* flow{"BOOT"};
    std::uint32_t heap_capacity{}, heap_used{}, heap_arena{}, heap_free_arena{};
    std::uint32_t linear_capacity{}, linear_free{};
    bool captured{};
};
extern AllocationFailure allocation_failure;
// std::new_handler: records heap and linear-heap use in allocation_failure,
// then throws std::bad_alloc.
void capture_allocation_failure();

} // namespace starfox::platform_3ds
