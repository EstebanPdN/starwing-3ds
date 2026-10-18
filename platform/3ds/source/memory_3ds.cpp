#include "memory_3ds.hpp"

#include <3ds.h>
#include <malloc.h>
#include <new>

namespace starfox::platform_3ds {

AllocationFailure allocation_failure;

void capture_allocation_failure() {
    // Snapshot before unwinding; no allocation or file I/O.
    const auto heap = mallinfo();
    allocation_failure.heap_capacity = envGetHeapSize();
    allocation_failure.heap_used = static_cast<std::uint32_t>(heap.uordblks);
    allocation_failure.heap_arena = static_cast<std::uint32_t>(heap.arena);
    allocation_failure.heap_free_arena = static_cast<std::uint32_t>(heap.fordblks);
    allocation_failure.linear_capacity = envGetLinearHeapSize();
    allocation_failure.linear_free = linearSpaceFree();
    allocation_failure.captured = true;
    throw std::bad_alloc{};
}

} // namespace starfox::platform_3ds
