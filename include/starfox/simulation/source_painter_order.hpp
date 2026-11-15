#pragma once
#include <cstdint>
#include <vector>

// The cartridge's painter order for a list of objects. GameSimulation uses it
// for the ending sequence's draw order; tests/ending_tests.cpp checks it.

namespace starfox::simulation {
// The cartridge uses stable insertion and a wrapping 16-bit CMP/BPL, not a
// C++ total-order sort. Keep this presentation-only: never relink/remove the
// native object pool or recalculate the suspended task's camera.
// Returns the handles with larger depth(handle) first (drawn first); equal
// depths keep their input order.
template<class Range,class Depth>
auto source_painter_order(const Range& handles,Depth depth) {
    using Handle=typename Range::value_type;
    struct Entry {Handle handle;std::int16_t depth;};
    std::vector<Entry> entries;
    for(const auto handle:handles) {
        const Entry current{handle,depth(handle)};
        auto at=entries.begin();
        while(at!=entries.end()
            && !(std::uint16_t(std::uint16_t(at->depth)-std::uint16_t(current.depth))&0x8000U)) ++at;
        entries.insert(at,current);
    }
    std::vector<Handle> result;result.reserve(entries.size());
    for(const auto& entry:entries) result.push_back(entry.handle);
    return result;
}
}
